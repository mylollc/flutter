// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux/fl_compositor_hdr.h"

#include <epoxy/egl.h>
#include <epoxy/gl.h>

#include <fcntl.h>
#include <gbm.h>
#include <gdk/gdkwayland.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

#include "flutter/shell/platform/linux/fl_framebuffer.h"
#include "flutter/shell/platform/linux/wayland_vendor/color-management-v1-client-protocol.h"
#include "flutter/shell/platform/linux/wayland_vendor/linux-dmabuf-v1-client-protocol.h"
#include "flutter/shell/platform/linux/wayland_vendor/viewporter-client-protocol.h"

// DRM fourccs (avoid a libdrm include; these are the stable fourcc codes).
#define FL_FOURCC(a, b, c, d)                                     \
  ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | \
   ((uint32_t)(d) << 24))
// [16:16:16:16] R:G:B:A little-endian, IEEE half-float per channel.
#define FL_DRM_FORMAT_ABGR16161616F FL_FOURCC('A', 'B', '4', 'H')

// EGL device-query tokens (define defensively; the bullseye sysroot's eglext.h
// predates EGL_EXT_device_drm_render_node).
#ifndef EGL_DRM_RENDER_NODE_FILE_EXT
#define EGL_DRM_RENDER_NODE_FILE_EXT 0x3377
#endif
#ifndef EGL_DRM_DEVICE_FILE_EXT
#define EGL_DRM_DEVICE_FILE_EXT 0x3233
#endif
#ifndef EGL_DEVICE_EXT
#define EGL_DEVICE_EXT 0x322C
#endif

// Present ring depth. Was pinned to 2 while the present was a
// glBlitFramebuffer: iris/Arrow-Lake silently stopped honoring blits into the
// 3rd+ concurrent LINEAR dma-buf render target (GL reported success, memory
// stayed unwritten; glClear into the same FBO landed — a blit-path limit, not
// the target). The present is now a shader draw, which is expected to land
// where blits didn't (as glClear does). If the loading-screen stale-slot
// flicker ever reappears, the draw path did NOT lift the limit — drop this
// back to 2 and investigate (a Mesa-iris bug vs. this EGLImage/FBO setup is
// still an open question).
#define HDR_RING 3

// A single dma-buf slot: a GBM bo rendered into via an EGLImage-backed FBO, and
// presented as the same bo's wl_buffer. Heap-allocated so it survives a resize
// reallocation (moved to the `retired` list, freed on release) without
// invalidating its wl_buffer.release listener data.
typedef struct _HdrBuffer {
  struct _FlCompositorHDR* owner;
  struct gbm_bo* bo;
  // The display the EGLImage was created on. EGL images are display (not
  // context) resources, so teardown works without a current GL context —
  // dispose may run after the raster context is gone.
  EGLDisplay dpy;
  EGLImageKHR image;
  GLuint texture;
  GLuint fbo;
  struct wl_buffer* buffer;
  size_t w, h;  // allocated buffer dimensions (>= the content drawn into it)
  // Dimensions of the frame drawn into the buffer's top-left region for the
  // most recent present (written by present_layers before publishing the
  // slot; read by render() for the viewport source rect + damage).
  size_t content_w, content_h;
  gboolean busy;     // committed; the compositor holds it until release
  gboolean retired;  // dropped from the ring on resize; free on release
} HdrBuffer;

// One wl_output we bound (with its color-management extension object). The
// compositor references one of these in wl_surface.enter, and each one emits
// image_description_changed when its color state (HDR on/off, SDR-brightness
// slider, profile) changes.
typedef struct _HdrOutput {
  struct _FlCompositorHDR* owner;
  uint32_t global_name;  // wl_registry name, for global_remove matching
  struct wl_output* output;
  struct wp_color_management_output_v1* cm_output;
} HdrOutput;

struct _FlCompositorHDR {
  FlCompositor parent_instance;

  FlTaskRunner* task_runner;
  FlOpenGLManager* opengl_manager;
  GtkWidget* widget;  // FlView render widget (not owned)

  // Wayland — created on the main thread at construction.
  struct wl_display* display;
  struct wl_event_queue* queue;
  struct wl_registry* registry;  // kept alive for output hotplug
  struct wl_compositor* comp;
  struct wl_subcompositor* subcomp;
  struct zwp_linux_dmabuf_v1* dmabuf;
  struct wp_viewporter* viewporter;
  struct wl_surface* parent;  // GDK toplevel surface (borrowed)
  struct wl_surface* surface;
  struct wl_subsurface* subsurface;
  struct wp_viewport* viewport;
  gboolean wl_ok;
  gboolean disposed;

  // Color management (wp_color_manager_v1). All CM state below is owned by
  // the thread draining `queue`: the main thread during hdr_setup (blocking
  // roundtrips), the raster thread afterwards (present_layers drains) — never
  // both concurrently, so it needs no lock.
  struct wp_color_manager_v1* cm;
  struct wp_color_management_surface_v1* cm_surface;
  GPtrArray* outputs;         // HdrOutput*
  HdrOutput* current_output;  // output the surface is on (default: first)

  // Advertised manager capabilities we require.
  gboolean cap_parametric;
  gboolean cap_set_luminances;
  gboolean cap_ext_linear;
  gboolean cap_srgb_primaries;
  gboolean cap_perceptual;

  // Live luminances of the current output's image description.
  // KWin anchors extended-linear electrical 1.0 at max_lum (clamping above),
  // NOT at reference_lum — so Skia's linear 1.0 = SDR-white frames must be
  // scaled by reference/max at present time to land SDR white on the desktop
  // white and give highlights the (max/reference) headroom above it.
  uint32_t out_min_lum;  // units of 0.0001 cd/m2 (protocol encoding)
  uint32_t out_max_lum;  // cd/m2
  uint32_t out_ref_lum;  // cd/m2
  // reference/max; 1.0 until known. Stored as float bits: written by
  // info_done (raster present dispatch OR the main-thread dispatch tick),
  // read by the raster draw — hence atomic.
  gint present_scale_bits;

  // max/reference in thousandths, read cross-thread (plugins poll it via
  // fl_view_get_display_headroom to drive HDR tone mapping) — hence atomic.
  gint headroom_milli;

  // A "headroom-changed" emission is queued on the main thread (guarded by
  // mutex; coalesces bursts of output-description reads into one emission).
  gboolean headroom_notify_pending;

  // Serializes dispatch of the private queue between the raster thread
  // (present) and the main-loop source, so the Wayland listeners never run
  // concurrently with themselves.
  GMutex dispatch_mutex;
  // Main-loop source that dispatches the private queue while no frames
  // present (see HdrQueueSource). 0 once removed.
  guint queue_source_id;

  // In-flight read of the current output's image description (a get_
  // image_description → ready → get_information → …events… → done chain,
  // advanced by queue dispatch — blocking at setup, per-frame drains after).
  struct wp_image_description_v1* pending_desc;
  struct wp_image_description_info_v1* pending_info;
  uint32_t pend_min_lum, pend_max_lum, pend_ref_lum;
  gboolean pend_lum_got;
  gboolean reread_needed;  // change arrived while a read was in flight

  // The surface's own image description (extended-linear at the output's
  // luminances). Recreated whenever the output luminances change;
  // `surface_desc_pending` is awaiting its ready event.
  struct wp_image_description_v1* surface_desc;
  struct wp_image_description_v1* surface_desc_pending;

  // Failure latches (raster thread). GBM device discovery is topology-bound
  // and never retried once it fails; ring-buffer allocation IS retried
  // (VRAM pressure can be transient) but warns once per failure episode.
  gboolean gbm_failed;
  gboolean alloc_warned;

  // Present-shader objects (raster thread, lazily created).
  GLuint program;
  GLint scale_location;
  GLint dst_offset_location;
  GLint dst_scale_location;
  GLuint vertex_buffer;
  gboolean program_failed;  // compile/link failed; fall back to blit

  struct gbm_device* gbm;
  int drm_fd;

  // Ring lives entirely on the raster thread (present_layers, its queue-drain
  // release handling, and the retired sweep). render() commits the pending
  // slot.
  HdrBuffer* ring[HDR_RING];
  GList* retired;  // HdrBuffer* awaiting release after a resize
  size_t ring_w, ring_h;

  // Synchronized-resize state (main thread only): the content size of the
  // last committed frame, and whether anything has been committed yet.
  // render() compares these against the widget's current size to decide
  // whether to wait for a matching frame (see the wait loop there).
  size_t last_content_w, last_content_h;
  gboolean committed_once;
  // A target size whose wait already timed out once — don't wait for it
  // again (a systematic engine-vs-widget size formula mismatch must cost one
  // hiccup, not one per frame). Cleared when a wait succeeds.
  size_t wait_failed_w, wait_failed_h;

  // Present handoff. present_layers (raster) draws into a ring slot and
  // publishes `pending_slot`; render() (main thread) does the Wayland
  // attach/commit of that slot 1:1 with the parent surface's commit, so the two
  // surfaces latch atomically. `busy` is written by render() (commit) and the
  // release callback, read by present_layers (slot pick) — all under `mutex`.
  GMutex mutex;
  int pending_slot;  // -1 = nothing waiting; else ring index drawn & ready
};

G_DEFINE_TYPE(FlCompositorHDR, fl_compositor_hdr, fl_compositor_get_type())

enum { SIGNAL_HEADROOM_CHANGED, LAST_SIGNAL };

static guint fl_compositor_hdr_signals[LAST_SIGNAL];

static float hdr_get_present_scale(FlCompositorHDR* self) {
  gint bits = g_atomic_int_get(&self->present_scale_bits);
  float value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

static void hdr_set_present_scale(FlCompositorHDR* self, float value) {
  gint bits;
  memcpy(&bits, &value, sizeof(bits));
  g_atomic_int_set(&self->present_scale_bits, bits);
}

static void hdr_start_output_read(FlCompositorHDR* self);
static void hdr_retag_surface(FlCompositorHDR* self);
static void hdr_output_add(FlCompositorHDR* self,
                           uint32_t name,
                           struct wl_output* output);
static void hdr_output_free(gpointer data);

// ---------------------------------------------------------------------------
// registry
// ---------------------------------------------------------------------------
static void registry_global(void* data,
                            struct wl_registry* reg,
                            uint32_t name,
                            const char* iface,
                            uint32_t version) {
  FlCompositorHDR* self = FL_COMPOSITOR_HDR(data);
  if (strcmp(iface, "wl_compositor") == 0) {
    self->comp = static_cast<struct wl_compositor*>(
        wl_registry_bind(reg, name, &wl_compositor_interface, 4));
  } else if (strcmp(iface, "wl_subcompositor") == 0) {
    self->subcomp = static_cast<struct wl_subcompositor*>(
        wl_registry_bind(reg, name, &wl_subcompositor_interface, 1));
  } else if (strcmp(iface, "zwp_linux_dmabuf_v1") == 0) {
    uint32_t v = version < 3 ? version : 3;
    self->dmabuf = static_cast<struct zwp_linux_dmabuf_v1*>(
        wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface, v));
  } else if (strcmp(iface, "wp_viewporter") == 0) {
    self->viewporter = static_cast<struct wp_viewporter*>(
        wl_registry_bind(reg, name, &wp_viewporter_interface, 1));
  } else if (strcmp(iface, "wp_color_manager_v1") == 0) {
    self->cm = static_cast<struct wp_color_manager_v1*>(
        wl_registry_bind(reg, name, &wp_color_manager_v1_interface, 1));
  } else if (strcmp(iface, "wl_output") == 0) {
    uint32_t v = version < 2 ? version : 2;
    struct wl_output* output = static_cast<struct wl_output*>(
        wl_registry_bind(reg, name, &wl_output_interface, v));
    hdr_output_add(self, name, output);
  }
}

// Output unplugged (dock/monitor hotplug). Dispatched on whichever thread
// drains the queue (raster, post-setup).
static void registry_global_remove(void* data,
                                   struct wl_registry* reg,
                                   uint32_t name) {
  FlCompositorHDR* self = FL_COMPOSITOR_HDR(data);
  if (self->outputs == nullptr) {
    return;
  }
  for (guint i = 0; i < self->outputs->len; i++) {
    HdrOutput* o = static_cast<HdrOutput*>(g_ptr_array_index(self->outputs, i));
    if (o->global_name != name) {
      continue;
    }
    gboolean was_current = (self->current_output == o);
    g_ptr_array_remove_index(self->outputs, i);  // frees via hdr_output_free
    if (was_current) {
      self->current_output =
          self->outputs->len > 0
              ? static_cast<HdrOutput*>(g_ptr_array_index(self->outputs, 0))
              : nullptr;
      if (self->current_output != nullptr) {
        hdr_start_output_read(self);
      }
    }
    return;
  }
}
static const struct wl_registry_listener registry_listener = {
    registry_global, registry_global_remove};

static void dmabuf_format(void* d, struct zwp_linux_dmabuf_v1* o, uint32_t f) {}
static void dmabuf_modifier(void* d,
                            struct zwp_linux_dmabuf_v1* o,
                            uint32_t f,
                            uint32_t hi,
                            uint32_t lo) {}
static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
    dmabuf_format, dmabuf_modifier};

// ---------------------------------------------------------------------------
// wp_color_manager_v1 capability events
// ---------------------------------------------------------------------------
static void cm_supported_intent(void* data,
                                struct wp_color_manager_v1* cm,
                                uint32_t render_intent) {
  FlCompositorHDR* self = FL_COMPOSITOR_HDR(data);
  if (render_intent == WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL) {
    self->cap_perceptual = TRUE;
  }
}
static void cm_supported_feature(void* data,
                                 struct wp_color_manager_v1* cm,
                                 uint32_t feature) {
  FlCompositorHDR* self = FL_COMPOSITOR_HDR(data);
  if (feature == WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC) {
    self->cap_parametric = TRUE;
  }
  if (feature == WP_COLOR_MANAGER_V1_FEATURE_SET_LUMINANCES) {
    self->cap_set_luminances = TRUE;
  }
}
static void cm_supported_tf_named(void* data,
                                  struct wp_color_manager_v1* cm,
                                  uint32_t tf) {
  FlCompositorHDR* self = FL_COMPOSITOR_HDR(data);
  if (tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR) {
    self->cap_ext_linear = TRUE;
  }
}
static void cm_supported_primaries_named(void* data,
                                         struct wp_color_manager_v1* cm,
                                         uint32_t primaries) {
  FlCompositorHDR* self = FL_COMPOSITOR_HDR(data);
  if (primaries == WP_COLOR_MANAGER_V1_PRIMARIES_SRGB) {
    self->cap_srgb_primaries = TRUE;
  }
}
static void cm_done(void* data, struct wp_color_manager_v1* cm) {}
static const struct wp_color_manager_v1_listener cm_listener = {
    cm_supported_intent, cm_supported_feature, cm_supported_tf_named,
    cm_supported_primaries_named, cm_done};

// ---------------------------------------------------------------------------
// image-description events. One listener serves both in-flight objects; the
// object pointer routes: `pending_desc` is an output description being read,
// `surface_desc_pending` is our own extended-linear description awaiting
// ready before it can be set on the surface.
// ---------------------------------------------------------------------------
static void info_done(void* data, struct wp_image_description_info_v1* info);
static void info_icc_file(void* data,
                          struct wp_image_description_info_v1* info,
                          int32_t fd,
                          uint32_t size) {
  if (fd >= 0) {
    close(fd);
  }
}
static void info_primaries(void* data,
                           struct wp_image_description_info_v1* info,
                           int32_t rx,
                           int32_t ry,
                           int32_t gx,
                           int32_t gy,
                           int32_t bx,
                           int32_t by,
                           int32_t wx,
                           int32_t wy) {}
static void info_primaries_named(void* data,
                                 struct wp_image_description_info_v1* info,
                                 uint32_t primaries) {}
static void info_tf_power(void* data,
                          struct wp_image_description_info_v1* info,
                          uint32_t eexp) {}
static void info_tf_named(void* data,
                          struct wp_image_description_info_v1* info,
                          uint32_t tf) {}
static void info_luminances(void* data,
                            struct wp_image_description_info_v1* info,
                            uint32_t min_lum,
                            uint32_t max_lum,
                            uint32_t reference_lum) {
  FlCompositorHDR* self = FL_COMPOSITOR_HDR(data);
  self->pend_min_lum = min_lum;
  self->pend_max_lum = max_lum;
  self->pend_ref_lum = reference_lum;
  self->pend_lum_got = TRUE;
}
static void info_target_primaries(void* data,
                                  struct wp_image_description_info_v1* info,
                                  int32_t rx,
                                  int32_t ry,
                                  int32_t gx,
                                  int32_t gy,
                                  int32_t bx,
                                  int32_t by,
                                  int32_t wx,
                                  int32_t wy) {}
static void info_target_luminance(void* data,
                                  struct wp_image_description_info_v1* info,
                                  uint32_t min_lum,
                                  uint32_t max_lum) {}
static void info_target_max_cll(void* data,
                                struct wp_image_description_info_v1* info,
                                uint32_t max_cll) {}
static void info_target_max_fall(void* data,
                                 struct wp_image_description_info_v1* info,
                                 uint32_t max_fall) {}
static const struct wp_image_description_info_v1_listener info_listener = {
    info_done,
    info_icc_file,
    info_primaries,
    info_primaries_named,
    info_tf_power,
    info_tf_named,
    info_luminances,
    info_target_primaries,
    info_target_luminance,
    info_target_max_cll,
    info_target_max_fall};

static void desc_failed(void* data,
                        struct wp_image_description_v1* desc,
                        uint32_t cause,
                        const char* msg) {
  FlCompositorHDR* self = FL_COMPOSITOR_HDR(data);
  g_warning("FlCompositorHDR: image description failed (cause=%u): %s", cause,
            msg != nullptr ? msg : "");
  if (desc == self->pending_desc) {
    wp_image_description_v1_destroy(self->pending_desc);
    self->pending_desc = nullptr;
    if (self->reread_needed) {
      self->reread_needed = FALSE;
      hdr_start_output_read(self);
    }
  } else if (desc == self->surface_desc_pending) {
    wp_image_description_v1_destroy(self->surface_desc_pending);
    self->surface_desc_pending = nullptr;
  }
}
static void desc_ready(void* data,
                       struct wp_image_description_v1* desc,
                       uint32_t identity) {
  FlCompositorHDR* self = FL_COMPOSITOR_HDR(data);
  if (desc == self->pending_desc) {
    // Output description ready: ask for its parameters.
    self->pend_min_lum = self->pend_max_lum = self->pend_ref_lum = 0;
    self->pend_lum_got = FALSE;
    self->pending_info = wp_image_description_v1_get_information(desc);
    wp_image_description_info_v1_add_listener(self->pending_info,
                                              &info_listener, self);
  } else if (desc == self->surface_desc_pending) {
    // Our extended-linear description is usable: apply it. It becomes the
    // surface's committed state at the next render() commit.
    wp_color_management_surface_v1_set_image_description(
        self->cm_surface, desc, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
    if (self->surface_desc != nullptr) {
      wp_image_description_v1_destroy(self->surface_desc);
    }
    self->surface_desc = desc;
    self->surface_desc_pending = nullptr;
  }
}
static void desc_ready2(void* data,
                        struct wp_image_description_v1* desc,
                        uint32_t identity_hi,
                        uint32_t identity_lo) {
  desc_ready(data, desc, identity_lo);
}
static const struct wp_image_description_v1_listener desc_listener = {
    desc_failed, desc_ready, desc_ready2};

// Main-thread half of the headroom-change notification: emit the signal so
// FlView (and through it, application code) can react — e.g. re-render HDR
// content tone-mapped to the new headroom. Consumers read the new value via
// fl_compositor_hdr_get_display_headroom.
static gboolean headroom_notify_idle(gpointer data) {
  FlCompositorHDR* self = FL_COMPOSITOR_HDR(data);
  g_mutex_lock(&self->mutex);
  self->headroom_notify_pending = FALSE;
  gboolean disposed = self->disposed;
  g_mutex_unlock(&self->mutex);
  if (!disposed) {
    g_signal_emit(self, fl_compositor_hdr_signals[SIGNAL_HEADROOM_CHANGED], 0);
  }
  g_object_unref(self);
  return G_SOURCE_REMOVE;
}

// Raster-thread half: the output-description read runs on the private queue
// dispatched during present, so hop to the main thread (holding a ref) to
// emit. Coalesces — at most one emission is in flight at a time.
static void hdr_schedule_headroom_notify(FlCompositorHDR* self) {
  g_mutex_lock(&self->mutex);
  gboolean skip = self->headroom_notify_pending || self->disposed;
  if (!skip) {
    self->headroom_notify_pending = TRUE;
  }
  g_mutex_unlock(&self->mutex);
  if (!skip) {
    g_idle_add(headroom_notify_idle, g_object_ref(self));
  }
}

// End of an output-description read: adopt the values, rescale, retag.
static void info_done(void* data, struct wp_image_description_info_v1* info) {
  FlCompositorHDR* self = FL_COMPOSITOR_HDR(data);
  if (info != self->pending_info) {
    return;
  }
  wp_image_description_info_v1_destroy(self->pending_info);
  self->pending_info = nullptr;
  if (self->pending_desc != nullptr) {
    wp_image_description_v1_destroy(self->pending_desc);
    self->pending_desc = nullptr;
  }

  if (self->pend_lum_got && self->pend_max_lum > 0 && self->pend_ref_lum > 0) {
    gboolean changed = self->pend_min_lum != self->out_min_lum ||
                       self->pend_max_lum != self->out_max_lum ||
                       self->pend_ref_lum != self->out_ref_lum;
    self->out_min_lum = self->pend_min_lum;
    self->out_max_lum = self->pend_max_lum;
    self->out_ref_lum = self->pend_ref_lum;
    float present_scale = (float)self->out_ref_lum / (float)self->out_max_lum;
    hdr_set_present_scale(self, present_scale);
    gint headroom_milli =
        (gint)((1000.0 * self->out_max_lum) / self->out_ref_lum);
    gboolean headroom_changed =
        headroom_milli != g_atomic_int_get(&self->headroom_milli);
    g_atomic_int_set(&self->headroom_milli, headroom_milli);
    if (headroom_changed) {
      hdr_schedule_headroom_notify(self);
    }
    if (changed) {
      g_debug(
          "FlCompositorHDR: output luminances min=%.4f max=%u ref=%u cd/m2 "
          "(headroom %.2fx, present scale %.4f)",
          self->out_min_lum / 1e4, self->out_max_lum, self->out_ref_lum,
          (double)self->out_max_lum / self->out_ref_lum, present_scale);
      hdr_retag_surface(self);
    }
  } else {
    g_warning(
        "FlCompositorHDR: output image description carried no luminances; "
        "keeping previous values (scale %.4f)",
        hdr_get_present_scale(self));
  }

  if (self->reread_needed) {
    self->reread_needed = FALSE;
    hdr_start_output_read(self);
  }
}

// ---------------------------------------------------------------------------
// output color state
// ---------------------------------------------------------------------------
static void cm_output_changed(void* data,
                              struct wp_color_management_output_v1* cm_output) {
  HdrOutput* o = static_cast<HdrOutput*>(data);
  FlCompositorHDR* self = o->owner;
  // Only the output the surface is on drives the scale/tag. (KWin re-emits
  // this when HDR is toggled or the SDR-brightness slider moves.)
  if (self->current_output == o) {
    hdr_start_output_read(self);
  }
}
static const struct wp_color_management_output_v1_listener cm_output_listener =
    {cm_output_changed};

static void hdr_output_free(gpointer data) {
  HdrOutput* o = static_cast<HdrOutput*>(data);
  if (o->cm_output != nullptr) {
    wp_color_management_output_v1_destroy(o->cm_output);
  }
  if (o->output != nullptr) {
    wl_output_destroy(o->output);
  }
  g_free(o);
}

static void hdr_output_add(FlCompositorHDR* self,
                           uint32_t name,
                           struct wl_output* output) {
  HdrOutput* o = g_new0(HdrOutput, 1);
  o->owner = self;
  o->global_name = name;
  o->output = output;
  // During the initial registry roundtrip `cm` may not be bound yet (global
  // order is arbitrary); hdr_setup fills cm_output in afterwards. For hotplug
  // (post-setup) cm is available immediately.
  if (self->cm != nullptr) {
    o->cm_output = wp_color_manager_v1_get_output(self->cm, output);
    wp_color_management_output_v1_add_listener(o->cm_output,
                                               &cm_output_listener, o);
  }
  g_ptr_array_add(self->outputs, o);
  if (self->current_output == nullptr) {
    self->current_output = o;
  }
}

// Begin (or queue) a read of the current output's image description.
static void hdr_start_output_read(FlCompositorHDR* self) {
  if (self->current_output == nullptr ||
      self->current_output->cm_output == nullptr) {
    return;
  }
  if (self->pending_desc != nullptr || self->pending_info != nullptr) {
    self->reread_needed = TRUE;
    return;
  }
  self->pending_desc = wp_color_management_output_v1_get_image_description(
      self->current_output->cm_output);
  wp_image_description_v1_add_listener(self->pending_desc, &desc_listener,
                                       self);
}

// (Re)create the surface's extended-linear image description at the current
// output luminances. Applied asynchronously when its ready event arrives.
static void hdr_retag_surface(FlCompositorHDR* self) {
  if (self->cm_surface == nullptr || self->out_max_lum == 0 ||
      self->out_ref_lum == 0) {
    return;
  }
  if (self->surface_desc_pending != nullptr) {
    // A retag is already in flight; the newest luminances will win because
    // info_done calls us again only on change — drop the stale attempt.
    wp_image_description_v1_destroy(self->surface_desc_pending);
    self->surface_desc_pending = nullptr;
  }
  struct wp_image_description_creator_params_v1* creator =
      wp_color_manager_v1_create_parametric_creator(self->cm);
  wp_image_description_creator_params_v1_set_tf_named(
      creator, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR);
  wp_image_description_creator_params_v1_set_primaries_named(
      creator, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB);
  // min_lum is in 0.0001 cd/m2 units; max/reference in cd/m2. Declare the
  // output's own volume: we pre-scale pixels by reference/max, so our content
  // exactly fills [0, max] with SDR white at reference.
  wp_image_description_creator_params_v1_set_luminances(
      creator, self->out_min_lum, self->out_max_lum, self->out_ref_lum);
  self->surface_desc_pending =
      wp_image_description_creator_params_v1_create(creator);
  wp_image_description_v1_add_listener(self->surface_desc_pending,
                                       &desc_listener, self);
}

// ---------------------------------------------------------------------------
// wl_surface enter/leave — which output is the window on? The compositor
// sends enter per bound wl_output object; only references to OUR bindings
// match (GDK's bindings for the same globals are ignored).
// ---------------------------------------------------------------------------
static void surface_enter(void* data,
                          struct wl_surface* surface,
                          struct wl_output* output) {
  FlCompositorHDR* self = FL_COMPOSITOR_HDR(data);
  for (guint i = 0; i < self->outputs->len; i++) {
    HdrOutput* o = static_cast<HdrOutput*>(g_ptr_array_index(self->outputs, i));
    if (o->output == output) {
      if (self->current_output != o) {
        self->current_output = o;
        hdr_start_output_read(self);
      }
      return;
    }
  }
}
static void surface_leave(void* data,
                          struct wl_surface* surface,
                          struct wl_output* output) {}
static const struct wl_surface_listener surface_listener = {surface_enter,
                                                            surface_leave};

// ---------------------------------------------------------------------------
// buffer lifecycle
// ---------------------------------------------------------------------------
// `have_gl` = a GL context is current. GL names (FBO/texture) can only be
// deleted with a context; when none exists (dispose after context teardown)
// they die with the context, and everything else — the EGL image (a display
// resource), the wl_buffer, the gbm_bo — is still freed.
static void hdr_buffer_free(HdrBuffer* b, gboolean have_gl) {
  if (!b) {
    return;
  }
  if (have_gl) {
    if (b->fbo) {
      glDeleteFramebuffers(1, &b->fbo);
    }
    if (b->texture) {
      glDeleteTextures(1, &b->texture);
    }
  }
  if (b->image != EGL_NO_IMAGE_KHR) {
    // Don't call eglDestroyImageKHR through epoxy: epoxy resolves EGL
    // extension symbols against the current display, and the window-close
    // dispose path has none, so its dispatch would abort(). A raw
    // eglGetProcAddress pointer (core EGL, resolvable with no display)
    // only needs b->dpy to be valid.
    PFNEGLDESTROYIMAGEKHRPROC destroy_image =
        reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
            eglGetProcAddress("eglDestroyImageKHR"));
    if (destroy_image != nullptr) {
      destroy_image(b->dpy, b->image);
    }
  }
  if (b->buffer) {
    wl_buffer_destroy(b->buffer);
  }
  if (b->bo) {
    gbm_bo_destroy(b->bo);
  }
  g_free(b);
}

// wl_buffer.release — dispatched on the raster thread (queue drained in
// present_layers). Clears `busy`; a retired buffer's GL objects are freed later
// in hdr_sweep_retired (raster thread), where the owning GL context is current.
static void buffer_release(void* data, struct wl_buffer* buffer) {
  HdrBuffer* b = static_cast<HdrBuffer*>(data);
  // `busy` is also set by render() on the main thread — guard it.
  g_mutex_lock(&b->owner->mutex);
  b->busy = FALSE;
  g_mutex_unlock(&b->owner->mutex);
}
static const struct wl_buffer_listener buffer_listener = {buffer_release};

// Free retired buffers the compositor has released. Raster-thread-only (GL
// deletes need the raster context); called from present_layers.
static void hdr_sweep_retired(FlCompositorHDR* self) {
  GList* keep = nullptr;
  for (GList* l = self->retired; l; l = l->next) {
    HdrBuffer* b = static_cast<HdrBuffer*>(l->data);
    g_mutex_lock(&self->mutex);
    gboolean busy = b->busy;
    g_mutex_unlock(&self->mutex);
    if (busy) {
      keep = g_list_prepend(keep, b);
    } else {
      // GL deletes on the raster context (this thread).
      hdr_buffer_free(b, TRUE);
    }
  }
  g_list_free(self->retired);
  self->retired = keep;
}

static HdrBuffer* hdr_buffer_new(FlCompositorHDR* self, size_t w, size_t h) {
  HdrBuffer* b = g_new0(HdrBuffer, 1);
  b->owner = self;
  b->image = EGL_NO_IMAGE_KHR;
  b->w = w;
  b->h = h;

  b->bo = gbm_bo_create(self->gbm, w, h, FL_DRM_FORMAT_ABGR16161616F,
                        GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
  if (!b->bo) {
    if (!self->alloc_warned) {
      self->alloc_warned = TRUE;
      g_warning("FlCompositorHDR: gbm_bo_create %zux%zu (F16) failed", w, h);
    }
    hdr_buffer_free(b, TRUE);
    return nullptr;
  }
  uint32_t stride = gbm_bo_get_stride(b->bo);
  uint32_t offset = gbm_bo_get_offset(b->bo, 0);

  EGLDisplay dpy = eglGetCurrentDisplay();
  b->dpy = dpy;
  int fd_egl = gbm_bo_get_fd(b->bo);
  EGLint attrs[] = {EGL_WIDTH,
                    (EGLint)w,
                    EGL_HEIGHT,
                    (EGLint)h,
                    EGL_LINUX_DRM_FOURCC_EXT,
                    (EGLint)FL_DRM_FORMAT_ABGR16161616F,
                    EGL_DMA_BUF_PLANE0_FD_EXT,
                    fd_egl,
                    EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                    (EGLint)offset,
                    EGL_DMA_BUF_PLANE0_PITCH_EXT,
                    (EGLint)stride,
                    EGL_NONE};
  b->image = eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                               (EGLClientBuffer)NULL, attrs);
  close(fd_egl);
  if (b->image == EGL_NO_IMAGE_KHR) {
    if (!self->alloc_warned) {
      self->alloc_warned = TRUE;
      g_warning("FlCompositorHDR: eglCreateImageKHR (F16) failed egl=0x%x",
                eglGetError());
    }
    hdr_buffer_free(b, TRUE);
    return nullptr;
  }

  glGenTextures(1, &b->texture);
  glBindTexture(GL_TEXTURE_2D, b->texture);
  glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)b->image);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  glGenFramebuffers(1, &b->fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, b->fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         b->texture, 0);
  gboolean complete =
      glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (!complete) {
    if (!self->alloc_warned) {
      self->alloc_warned = TRUE;
      g_warning("FlCompositorHDR: F16 dma-buf FBO incomplete");
    }
    hdr_buffer_free(b, TRUE);
    return nullptr;
  }

  int fd_wl = gbm_bo_get_fd(b->bo);
  struct zwp_linux_buffer_params_v1* params =
      zwp_linux_dmabuf_v1_create_params(self->dmabuf);
  zwp_linux_buffer_params_v1_add(params, fd_wl, 0, offset, stride, 0, 0);
  b->buffer = zwp_linux_buffer_params_v1_create_immed(
      params, w, h, FL_DRM_FORMAT_ABGR16161616F, 0);
  zwp_linux_buffer_params_v1_destroy(params);
  close(fd_wl);
  if (!b->buffer) {
    if (!self->alloc_warned) {
      self->alloc_warned = TRUE;
      g_warning("FlCompositorHDR: zwp_linux_buffer_params create_immed failed");
    }
    hdr_buffer_free(b, TRUE);
    return nullptr;
  }
  wl_buffer_add_listener(b->buffer, &buffer_listener, b);
  return b;
}

// ---------------------------------------------------------------------------
// GBM device on the GL context's own render node (raster thread) — no
// cross-GPU write; KWin imports the result for the display (standard PRIME).
// ---------------------------------------------------------------------------
// Try to adopt `node` as the GBM allocation device: open it, create the GBM
// device, and VALIDATE it by allocating (and freeing) a minimal buffer of the
// ring's actual format — a device whose GBM backend cannot allocate our
// buffers (e.g. the NVIDIA proprietary driver's allocator, which rejects
// LINEAR render buffers of these formats) is rejected here instead of
// failing on every frame later. On success the fd/device are stored on
// `self`; on failure everything is cleaned up and FALSE is returned.
static gboolean hdr_gbm_try_node(FlCompositorHDR* self, const char* node) {
  int fd = open(node, O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    return FALSE;
  }
  struct gbm_device* gbm = gbm_create_device(fd);
  if (gbm == nullptr) {
    close(fd);
    return FALSE;
  }
  struct gbm_bo* probe =
      gbm_bo_create(gbm, 16, 16, FL_DRM_FORMAT_ABGR16161616F,
                    GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
  if (probe == nullptr) {
    gbm_device_destroy(gbm);
    close(fd);
    return FALSE;
  }
  gbm_bo_destroy(probe);
  self->drm_fd = fd;
  self->gbm = gbm;
  g_debug("FlCompositorHDR: GBM allocation device = %s (backend %s)", node,
          gbm_device_get_backend_name(self->gbm));
  return TRUE;
}

static gboolean hdr_gbm_ensure(FlCompositorHDR* self) {
  if (self->gbm) {
    return TRUE;
  }
  if (self->gbm_failed) {
    return FALSE;  // topology-bound; warned once at latch time
  }
  // First choice: the node of the GL context's own device (same-GPU
  // allocation, no cross-device import).
  EGLDisplay dpy = eglGetCurrentDisplay();
  if (dpy != EGL_NO_DISPLAY &&
      epoxy_has_egl_extension(dpy, "EGL_EXT_device_query")) {
    EGLAttrib dev = 0;
    if (eglQueryDisplayAttribEXT(dpy, EGL_DEVICE_EXT, &dev) && dev) {
      const char* node = eglQueryDeviceStringEXT((EGLDeviceEXT)dev,
                                                 EGL_DRM_RENDER_NODE_FILE_EXT);
      if (node == nullptr) {
        node =
            eglQueryDeviceStringEXT((EGLDeviceEXT)dev, EGL_DRM_DEVICE_FILE_EXT);
      }
      if (node != nullptr && hdr_gbm_try_node(self, node)) {
        return TRUE;
      }
    }
  }
  // Fallback: scan the render nodes and adopt the first whose GBM backend
  // passes the allocation probe. DRM node numbers are assigned in device
  // enumeration order, which is NOT stable across boots (a hybrid box can
  // swap renderD128/renderD129 on reboot), so a fixed node name is never
  // correct here. LINEAR buffers import across devices, so a non-GL-device
  // allocator still composites correctly. Render nodes occupy DRM minors
  // 128-191; probing an absent node fails fast in open().
  for (int i = 128; i < 192; i++) {
    g_autofree gchar* node = g_strdup_printf("/dev/dri/renderD%d", i);
    if (hdr_gbm_try_node(self, node)) {
      return TRUE;
    }
  }
  g_warning(
      "FlCompositorHDR: no render node with a GBM backend that can allocate "
      "the present ring (F16 linear) — falling back");
  self->gbm_failed = TRUE;
  return FALSE;
}

// Round a buffer dimension up so an interactive grow-drag reallocates the
// ring a handful of times (once per 256-px band) instead of once per pixel.
static size_t hdr_round_up_dim(size_t dim) {
  return (dim + 255) & ~(size_t)255;
}

// Ensure the ring can hold a w x h frame (raster thread, GL context current).
//
// With a viewport (wp_viewporter), buffers are GROW-ONLY: they are allocated
// with rounded-up headroom and kept when the frame shrinks — the frame draws
// into the top-left content region and render() crops it with
// wp_viewport_set_source. This is what keeps interactive resize fluid: the
// old exact-size model reallocated 3 F16 GBM buffers + EGLImages + FBOs at
// EVERY intermediate drag size, so content visibly lagged the frame. The
// memory cost of the high-water mark (a few hundred MB at 4K F16) is
// deliberate; buffers are freed on dispose. Without a viewport there is no
// crop, so the ring stays exact-size (every resize reallocates, as before).
//
// On a reallocation, in-flight (busy) buffers are RETIRED (kept alive until
// their release) rather than freed, so the main thread can safely still be
// presenting one; non-busy buffers are freed immediately.
static gboolean hdr_ring_ensure(FlCompositorHDR* self, size_t w, size_t h) {
  if (!hdr_gbm_ensure(self)) {
    return FALSE;
  }
  if (self->ring[0] && self->ring_w == w && self->ring_h == h) {
    return TRUE;
  }
  if (self->viewport) {
    if (self->ring[0] && w <= self->ring_w && h <= self->ring_h) {
      return TRUE;  // frame fits the existing buffers; crop presents it
    }
    // Grow-only: never shrink an axis, round the growing axis up.
    w = hdr_round_up_dim(MAX(w, self->ring_w));
    h = hdr_round_up_dim(MAX(h, self->ring_h));
  }
  // Size changed: retire in-flight buffers (freed on release, in the sweep),
  // free idle ones now. A slot the main thread is about to commit (busy, set by
  // render()) or has queued (pending_slot) must be retired, not freed — read
  // both under the lock since they are cross-thread. Clear a doomed pending so
  // render() skips it.
  g_mutex_lock(&self->mutex);
  int pend = self->pending_slot;
  self->pending_slot = -1;
  for (int i = 0; i < HDR_RING; i++) {
    HdrBuffer* b = self->ring[i];
    self->ring[i] = nullptr;
    if (!b) {
      continue;
    }
    if (b->busy || i == pend) {
      b->retired = TRUE;
      self->retired = g_list_prepend(self->retired, b);
    } else {
      hdr_buffer_free(b, TRUE);
    }
  }
  g_mutex_unlock(&self->mutex);
  self->ring_w = self->ring_h = 0;
  for (int i = 0; i < HDR_RING; i++) {
    self->ring[i] = hdr_buffer_new(self, w, h);
    if (!self->ring[i]) {
      return FALSE;
    }
  }
  self->alloc_warned = FALSE;  // recovered; a future episode warns again
  self->ring_w = w;
  self->ring_h = h;
  return TRUE;
}

// ---------------------------------------------------------------------------
// present shader — a fullscreen textured quad that Y-flips the backing store
// into the slot FBO and applies the whole-surface reference/max scale.
//
// A draw (not glBlitFramebuffer) for two reasons: the scale must be applied
// per-pixel to EVERY composited pixel (UI chrome included — KWin anchors
// extended-linear 1.0 at max_lum for the whole surface, so unscaled white UI
// would display at panel peak), and blits into the 3rd+ concurrent LINEAR
// dma-buf render target silently no-op on iris/Arrow-Lake (the HDR_RING=2
// workaround this path retires).
// ---------------------------------------------------------------------------
static const char* present_vertex_src =
    "attribute vec2 position;\n"
    "attribute vec2 in_texcoord;\n"
    "uniform vec2 dst_offset;\n"
    "uniform vec2 dst_scale;\n"
    "varying vec2 texcoord;\n"
    "\n"
    "void main() {\n"
    "  vec2 p = dst_offset + position * dst_scale;\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0, 1);\n"
    "  texcoord = in_texcoord;\n"
    "}\n";

static const char* present_fragment_src =
    "#ifdef GL_ES\n"
    "precision mediump float;\n"
    "#endif\n"
    "\n"
    "uniform sampler2D texture;\n"
    "uniform float out_scale;\n"
    "varying vec2 texcoord;\n"
    "\n"
    "void main() {\n"
    "  vec4 c = texture2D(texture, texcoord);\n"
    "  gl_FragColor = vec4(c.rgb * out_scale, c.a);\n"
    "}\n";

static gchar* hdr_get_shader_log(GLuint shader) {
  GLint log_length = 0;
  glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
  gchar* log = static_cast<gchar*>(g_malloc(log_length + 1));
  glGetShaderInfoLog(shader, log_length, nullptr, log);
  return log;
}

// Compile/link the present program (raster thread, GL context current).
static gboolean hdr_program_ensure(FlCompositorHDR* self) {
  if (self->program != 0) {
    return TRUE;
  }
  if (self->program_failed) {
    return FALSE;
  }

  GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(vertex_shader, 1, &present_vertex_src, nullptr);
  glCompileShader(vertex_shader);
  GLint vertex_status = GL_FALSE;
  glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &vertex_status);

  GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(fragment_shader, 1, &present_fragment_src, nullptr);
  glCompileShader(fragment_shader);
  GLint fragment_status = GL_FALSE;
  glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &fragment_status);

  GLuint program = 0;
  GLint link_status = GL_FALSE;
  if (vertex_status == GL_TRUE && fragment_status == GL_TRUE) {
    program = glCreateProgram();
    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &link_status);
  }

  if (vertex_status != GL_TRUE) {
    g_autofree gchar* log = hdr_get_shader_log(vertex_shader);
    g_warning("FlCompositorHDR: vertex shader failed: %s", log);
  }
  if (fragment_status != GL_TRUE) {
    g_autofree gchar* log = hdr_get_shader_log(fragment_shader);
    g_warning("FlCompositorHDR: fragment shader failed: %s", log);
  }
  glDeleteShader(vertex_shader);
  glDeleteShader(fragment_shader);
  if (link_status != GL_TRUE) {
    g_warning("FlCompositorHDR: present program failed to link");
    if (program != 0) {
      glDeleteProgram(program);
    }
    self->program_failed = TRUE;
    return FALSE;
  }

  self->program = program;
  self->scale_location = glGetUniformLocation(program, "out_scale");
  self->dst_offset_location = glGetUniformLocation(program, "dst_offset");
  self->dst_scale_location = glGetUniformLocation(program, "dst_scale");
  GLint texture_location = glGetUniformLocation(program, "texture");

  // Unit quad (pos.xy in [0,1] — the vertex shader maps it through the
  // per-layer dst rect to NDC; layer 0 uses offset 0 / scale 1 =
  // fullscreen). The v coordinate is flipped relative to position so the
  // bottom-left-origin GL backing store lands top-row-first in the dma-buf,
  // which is what the Wayland buffer expects (same flip the previous
  // glBlitFramebuffer(0,0,w,h -> 0,h,w,0) did).
  GLfloat vertex_data[] = {
      0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 0,
  };
  GLint saved_array_buffer = 0;
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &saved_array_buffer);
  glGenBuffers(1, &self->vertex_buffer);
  glBindBuffer(GL_ARRAY_BUFFER, self->vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, sizeof(vertex_data), vertex_data,
               GL_STATIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, saved_array_buffer);

  GLint saved_program = 0;
  glGetIntegerv(GL_CURRENT_PROGRAM, &saved_program);
  glUseProgram(program);
  glUniform1i(texture_location, 0);  // sampler on texture unit 0
  glUseProgram(saved_program);
  return TRUE;
}

// Draw the backing store into the slot FBO. present_layers runs in Skia's
// raster GL context, so EVERY piece of state the draw touches is saved and
// restored — Skia must see its context untouched. (The old blit only needed
// scissor discipline; a draw is additionally subject to program, VAO, blend,
// depth/stencil, cull, color mask, viewport, sampler and sRGB-encode state —
// the comprehensive save/restore stock FlCompositorOpenGL uses, plus the
// ones a draw adds over a blit.)
static void hdr_present_draw(FlCompositorHDR* self,
                             const FlutterLayer** layers,
                             size_t layers_count,
                             HdrBuffer* dst,
                             size_t w,
                             size_t h) {
  gboolean is_desktop_gl = epoxy_is_desktop_gl();

  GLint saved_program = 0;
  glGetIntegerv(GL_CURRENT_PROGRAM, &saved_program);
  GLint saved_active_texture = 0;
  glGetIntegerv(GL_ACTIVE_TEXTURE, &saved_active_texture);
  glActiveTexture(GL_TEXTURE0);
  GLint saved_texture_binding = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved_texture_binding);
  GLint saved_sampler_binding = 0;
  glGetIntegerv(GL_SAMPLER_BINDING, &saved_sampler_binding);
  GLint saved_vao_binding = 0;
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &saved_vao_binding);
  GLint saved_array_buffer_binding = 0;
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &saved_array_buffer_binding);
  GLint saved_draw_framebuffer_binding = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &saved_draw_framebuffer_binding);
  GLint saved_read_framebuffer_binding = 0;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &saved_read_framebuffer_binding);
  GLint saved_viewport[4] = {0, 0, 0, 0};
  glGetIntegerv(GL_VIEWPORT, saved_viewport);
  GLboolean saved_color_mask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
  glGetBooleanv(GL_COLOR_WRITEMASK, saved_color_mask);
  GLboolean saved_scissor_test = glIsEnabled(GL_SCISSOR_TEST);
  GLboolean saved_blend = glIsEnabled(GL_BLEND);
  GLboolean saved_cull_face = glIsEnabled(GL_CULL_FACE);
  GLboolean saved_depth_test = glIsEnabled(GL_DEPTH_TEST);
  GLboolean saved_stencil_test = glIsEnabled(GL_STENCIL_TEST);
  GLboolean saved_framebuffer_srgb =
      is_desktop_gl ? glIsEnabled(GL_FRAMEBUFFER_SRGB) : GL_FALSE;

  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_STENCIL_TEST);
  if (is_desktop_gl) {
    // The dma-buf FBO is F16 (not an sRGB format), but leave nothing to
    // chance: the values written must stay linear, un-encoded.
    glDisable(GL_FRAMEBUFFER_SRGB);
  }
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

  glBindFramebuffer(GL_FRAMEBUFFER, dst->fbo);
  glViewport(0, 0, w, h);

  glUseProgram(self->program);
  glUniform1f(self->scale_location, hdr_get_present_scale(self));

  glBindSampler(0, 0);  // texture-object params (NEAREST), not a sampler's

  // Like stock FlCompositorOpenGL: VAOs can't be shared between contexts, so
  // build a transient one per present.
  GLuint vao = 0;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, self->vertex_buffer);
  GLint position_location = glGetAttribLocation(self->program, "position");
  glEnableVertexAttribArray(position_location);
  glVertexAttribPointer(position_location, 2, GL_FLOAT, GL_FALSE,
                        sizeof(GLfloat) * 4, 0);
  GLint texcoord_location = glGetAttribLocation(self->program, "in_texcoord");
  glEnableVertexAttribArray(texcoord_location);
  glVertexAttribPointer(texcoord_location, 2, GL_FLOAT, GL_FALSE,
                        sizeof(GLfloat) * 4,
                        reinterpret_cast<void*>(sizeof(GLfloat) * 2));

  // Composite every backing-store layer, stock-FlCompositorOpenGL parity:
  // the base layer fills the target opaquely; overlay layers alpha-blend at
  // their offsets. Layer placement is Y-flipped into the dma-buf's
  // top-row-first space (the same flip the quad's texcoords perform for the
  // pixels). Platform views are not implemented on Linux (upstream
  // flutter#41724), matching stock.
  gboolean first_layer = TRUE;
  for (size_t i = 0; i < layers_count; ++i) {
    const FlutterLayer* layer = layers[i];
    if (layer->type != kFlutterLayerContentTypeBackingStore) {
      continue;
    }
    FlFramebuffer* src =
        FL_FRAMEBUFFER(layer->backing_store->open_gl.framebuffer.user_data);
    if (first_layer) {
      glDisable(GL_BLEND);
      first_layer = FALSE;
    } else {
      glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    double lx = layer->offset.x;
    double ly = layer->offset.y;
    double lw = layer->size.width;
    double lh = layer->size.height;
    glUniform2f(self->dst_offset_location, lx / w, (h - ly - lh) / h);
    glUniform2f(self->dst_scale_location, lw / w, lh / h);
    glBindTexture(GL_TEXTURE_2D, fl_framebuffer_get_texture_id(src));
    glDrawArrays(GL_TRIANGLES, 0, 6);
  }
  glDisable(GL_BLEND);

  glDeleteVertexArrays(1, &vao);

  // Restore everything.
  glBindBuffer(GL_ARRAY_BUFFER, saved_array_buffer_binding);
  glBindVertexArray(saved_vao_binding);
  glBindTexture(GL_TEXTURE_2D, saved_texture_binding);
  glBindSampler(0, saved_sampler_binding);
  glActiveTexture(saved_active_texture);
  glUseProgram(saved_program);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, saved_draw_framebuffer_binding);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, saved_read_framebuffer_binding);
  glViewport(saved_viewport[0], saved_viewport[1], saved_viewport[2],
             saved_viewport[3]);
  glColorMask(saved_color_mask[0], saved_color_mask[1], saved_color_mask[2],
              saved_color_mask[3]);
  if (saved_scissor_test) {
    glEnable(GL_SCISSOR_TEST);
  }
  if (saved_blend) {
    glEnable(GL_BLEND);
  }
  if (saved_cull_face) {
    glEnable(GL_CULL_FACE);
  }
  if (saved_depth_test) {
    glEnable(GL_DEPTH_TEST);
  }
  if (saved_stencil_test) {
    glEnable(GL_STENCIL_TEST);
  }
  if (is_desktop_gl && saved_framebuffer_srgb) {
    glEnable(GL_FRAMEBUFFER_SRGB);
  }
}

// Last-resort present when the program failed to build: the step-2 blit
// (correct pixels except the missing reference/max scale — visibly too
// bright on an HDR output, but never blank). Scissor discipline per
// flutter#140828.
static void hdr_present_blit(FlFramebuffer* src,
                             HdrBuffer* dst,
                             size_t w,
                             size_t h) {
  GLint saved_read = 0, saved_draw = 0;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &saved_read);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &saved_draw);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fl_framebuffer_get_id(src));
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst->fbo);
  GLboolean saved_scissor = glIsEnabled(GL_SCISSOR_TEST);
  glDisable(GL_SCISSOR_TEST);
  glBlitFramebuffer(0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  if (saved_scissor) {
    glEnable(GL_SCISSOR_TEST);
  }
  glBindFramebuffer(GL_READ_FRAMEBUFFER, saved_read);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, saved_draw);
}

// ---------------------------------------------------------------------------
// FlCompositor::present_layers (raster thread) — draw the layer into a free
// ring slot's dma-buf FBO and publish it as `pending_slot`; render() (main
// thread) does the actual subsurface commit.
// ---------------------------------------------------------------------------
static gboolean fl_compositor_hdr_present_layers(FlCompositor* compositor,
                                                 const FlutterLayer** layers,
                                                 size_t layers_count) {
  FlCompositorHDR* self = FL_COMPOSITOR_HDR(compositor);
  if (!self->wl_ok || layers_count == 0) {
    return TRUE;
  }
  g_mutex_lock(&self->mutex);
  gboolean disposed = self->disposed;
  g_mutex_unlock(&self->mutex);
  if (disposed) {
    return TRUE;
  }
  // The base layer sizes the target; hdr_present_draw composites the full
  // layer list (platform views excluded, as in stock — flutter#41724).
  const FlutterLayer* layer = layers[0];
  if (layer->type != kFlutterLayerContentTypeBackingStore) {
    return TRUE;
  }
  size_t w = layer->size.width;
  size_t h = layer->size.height;

  // Drain buffer releases + color-management events (output luminance
  // changes, surface enter, description ready) — all on our private queue.
  // dispatch_mutex: the main-thread tick dispatches the same queue.
  g_mutex_lock(&self->dispatch_mutex);
  wl_display_dispatch_queue_pending(self->display, self->queue);
  g_mutex_unlock(&self->dispatch_mutex);
  hdr_sweep_retired(self);
  if (!hdr_ring_ensure(self, w, h)) {
    return TRUE;
  }

  // Pick a slot the compositor isn't holding (busy) and that isn't already
  // waiting for render() to commit it (pending_slot). busy/pending are
  // cross-thread — read under the lock.
  int slot = -1;
  g_mutex_lock(&self->mutex);
  int pend = self->pending_slot;
  for (int i = 0; i < HDR_RING; i++) {
    if (self->ring[i] && !self->ring[i]->busy && i != pend) {
      slot = i;
      break;
    }
  }
  g_mutex_unlock(&self->mutex);
  if (slot < 0) {
    return TRUE;  // all in flight; drop this frame (backpressure)
  }
  HdrBuffer* b = self->ring[slot];
  // Content dims for render()'s viewport source rect + damage (the buffer
  // may be larger — grow-only ring). Published to the main thread by the
  // pending_slot store below.
  b->content_w = w;
  b->content_h = h;

  // Composite: draw the layers into the bo-backed FBO (Y-flip +
  // reference/max scale). Blit fallback (base layer only) if the program
  // can't build.
  if (hdr_program_ensure(self)) {
    hdr_present_draw(self, layers, layers_count, b, w, h);
  } else {
    hdr_present_blit(
        FL_FRAMEBUFFER(layer->backing_store->open_gl.framebuffer.user_data), b,
        w, h);
  }
  // Submit the draw. No CPU-side wait is needed before handing the dma-buf to
  // the compositor: on Mesa the kernel attaches the submitted batch's fences
  // to the buffer's reservation object (dma-buf implicit sync), so the
  // compositor's own GPU work waits on the render automatically — the same
  // contract eglSwapBuffers relies on. The flush must happen before render()
  // commits the buffer; that ordering is guaranteed by the pending_slot
  // handoff below.
  glFlush();

  // Hand the drawn slot to render(): it does the wl_surface attach/commit on
  // the main thread, exactly once per parent-surface commit, so the subsurface
  // and parent latch atomically. If a prior pending slot was never committed
  // (render() hasn't run yet), it is simply superseded here — we always present
  // the newest frame.
  g_mutex_lock(&self->mutex);
  self->pending_slot = slot;
  g_mutex_unlock(&self->mutex);
  // Wake render() if it is blocked in the synchronized-resize wait — this
  // frame may be the matching-size one it needs (stock parity).
  fl_task_runner_stop_wait(self->task_runner);
  return TRUE;
}

// ---------------------------------------------------------------------------
// FlCompositor::render (main thread) — commit the slot present_layers drew
// onto the subsurface (1:1 with GDK's parent commit) and clear the GTK parent
// surface (the subsurface carries the pixels on top).
// ---------------------------------------------------------------------------
static gboolean fl_compositor_hdr_render(FlCompositor* compositor,
                                         cairo_t* cr,
                                         GdkWindow* window) {
  FlCompositorHDR* self = FL_COMPOSITOR_HDR(compositor);

  GtkWidget* toplevel = gtk_widget_get_toplevel(self->widget);
  int scale = gdk_window_get_scale_factor(window);
  gint ox = 0, oy = 0;
  gtk_widget_translate_coordinates(self->widget, toplevel, 0, 0, &ox, &oy);
  GtkAllocation alloc;
  gtk_widget_get_allocation(self->widget, &alloc);

  // Synchronized resize (stock FlCompositorOpenGL parity): when the widget
  // size changed, wait for the raster thread to deliver a frame at the NEW
  // size before committing — otherwise the previous frame gets
  // viewport-scaled to the new geometry and content visibly trails the frame
  // through an interactive resize. fl_task_runner_wait keeps the engine's
  // platform tasks running while blocked (the UI isolate shares this
  // thread), which is exactly what lets the new-size frame get built during
  // the wait; present_layers calls fl_task_runner_stop_wait on every
  // publish. Not synchronized until the first commit (startup), and capped —
  // unlike stock's unbounded wait — so a stalled engine degrades to a
  // scaled frame instead of a frozen window.
  if (self->committed_once && alloc.width > 0 && alloc.height > 0) {
    size_t want_w = (size_t)alloc.width * (scale > 0 ? scale : 1);
    size_t want_h = (size_t)alloc.height * (scale > 0 ? scale : 1);
    if ((self->last_content_w != want_w || self->last_content_h != want_h) &&
        !(self->wait_failed_w == want_w && self->wait_failed_h == want_h)) {
      gint64 deadline = g_get_monotonic_time() + 500 * G_TIME_SPAN_MILLISECOND;
      gboolean match = FALSE;
      while (!match && g_get_monotonic_time() < deadline) {
        g_mutex_lock(&self->mutex);
        int p = self->pending_slot;
        match = p >= 0 && self->ring[p] && self->ring[p]->content_w == want_w &&
                self->ring[p]->content_h == want_h;
        g_mutex_unlock(&self->mutex);
        if (!match) {
          fl_task_runner_wait(self->task_runner);
        }
      }
      if (match) {
        self->wait_failed_w = self->wait_failed_h = 0;
      } else {
        self->wait_failed_w = want_w;
        self->wait_failed_h = want_h;
      }
    }
  }

  // Commit the slot present_layers drew, here on the main thread — exactly
  // once per parent-surface commit (GDK issues the parent commit right after
  // this draw returns). In sync mode the subsurface's new buffer latches
  // ATOMICALLY with the parent commit, eliminating the raster-thread
  // double-commit-per-apply churn that flickered the animating opening screen.
  // Capture the buffer under the lock (mark it busy so the raster thread won't
  // reuse or free it); a concurrent resize retires it rather than freeing.
  g_mutex_lock(&self->mutex);
  int slot = self->pending_slot;
  self->pending_slot = -1;
  HdrBuffer* b = (slot >= 0 && self->ring[slot]) ? self->ring[slot] : nullptr;
  if (b) {
    b->busy = TRUE;
  }
  g_mutex_unlock(&self->mutex);
  if (b && b->buffer) {
    wl_subsurface_set_position(self->subsurface, ox, oy);
    if (self->viewport && alloc.width > 0 && alloc.height > 0) {
      // Present only the top-left content region: the grow-only ring keeps
      // buffers at their high-water size, so the frame may occupy a sub-rect.
      wp_viewport_set_source(self->viewport, wl_fixed_from_int(0),
                             wl_fixed_from_int(0),
                             wl_fixed_from_int((int)b->content_w),
                             wl_fixed_from_int((int)b->content_h));
      wp_viewport_set_destination(self->viewport, alloc.width, alloc.height);
    } else {
      wl_surface_set_buffer_scale(self->surface, scale > 0 ? scale : 1);
    }
    wl_surface_attach(self->surface, b->buffer, 0, 0);
    wl_surface_damage_buffer(self->surface, 0, 0, b->content_w, b->content_h);
    wl_surface_commit(self->surface);
    wl_display_flush(self->display);
    self->last_content_w = b->content_w;
    self->last_content_h = b->content_h;
    self->committed_once = TRUE;
  }

  // The render area is GL-backed; a cairo CLEAR may not actually wipe the GL
  // framebuffer GDK presents on the PARENT surface, leaving stale/garbage GL
  // content that shows through on dynamic frames (the opening-screen flicker).
  // draw_cb has made the render context current, so clear the GL framebuffer
  // to transparent explicitly. (Cairo clear kept for the non-GL/software path.)
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  cairo_save(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint(cr);
  cairo_restore(cr);
  return TRUE;
}

// ---------------------------------------------------------------------------
// one-time Wayland setup (main thread, at construction)
// ---------------------------------------------------------------------------
static gboolean hdr_setup(FlCompositorHDR* self) {
  GtkWidget* toplevel = gtk_widget_get_toplevel(self->widget);
  GdkWindow* win = gtk_widget_get_window(toplevel);
  GdkDisplay* gdisplay = gtk_widget_get_display(toplevel);
  if (!GDK_IS_WAYLAND_DISPLAY(gdisplay) || win == nullptr) {
    return FALSE;
  }
  self->display = gdk_wayland_display_get_wl_display(gdisplay);
  self->parent = gdk_wayland_window_get_wl_surface(win);
  if (!self->display || !self->parent) {
    return FALSE;
  }

  gint64 t0 = g_get_monotonic_time();

  self->queue = wl_display_create_queue(self->display);
  self->registry = wl_display_get_registry(self->display);
  wl_proxy_set_queue((struct wl_proxy*)self->registry, self->queue);
  wl_registry_add_listener(self->registry, &registry_listener, self);
  wl_display_roundtrip_queue(self->display, self->queue);
  gint64 t_registry = g_get_monotonic_time();
  if (!self->comp || !self->subcomp || !self->dmabuf || !self->cm ||
      self->outputs->len == 0) {
    g_warning(
        "FlCompositorHDR: required Wayland globals missing (wl_compositor/"
        "wl_subcompositor/zwp_linux_dmabuf_v1/wp_color_manager_v1/wl_output)");
    return FALSE;
  }
  zwp_linux_dmabuf_v1_add_listener(self->dmabuf, &dmabuf_listener, self);
  wp_color_manager_v1_add_listener(self->cm, &cm_listener, self);
  // The registry roundtrip may have delivered wl_output globals before
  // wp_color_manager_v1 — attach their color-management objects now.
  for (guint i = 0; i < self->outputs->len; i++) {
    HdrOutput* o = static_cast<HdrOutput*>(g_ptr_array_index(self->outputs, i));
    if (o->cm_output == nullptr) {
      o->cm_output = wp_color_manager_v1_get_output(self->cm, o->output);
      wp_color_management_output_v1_add_listener(o->cm_output,
                                                 &cm_output_listener, o);
    }
  }
  wl_display_roundtrip_queue(self->display, self->queue);  // caps
  gint64 t_caps = g_get_monotonic_time();
  if (!self->cap_parametric || !self->cap_set_luminances ||
      !self->cap_ext_linear || !self->cap_srgb_primaries ||
      !self->cap_perceptual) {
    g_warning(
        "FlCompositorHDR: wp_color_manager_v1 lacks required capabilities "
        "(parametric=%d set_luminances=%d ext_linear=%d srgb=%d "
        "perceptual=%d)",
        self->cap_parametric, self->cap_set_luminances, self->cap_ext_linear,
        self->cap_srgb_primaries, self->cap_perceptual);
    return FALSE;
  }

  // Initial (blocking) read of the current output's luminances, so the very
  // first present already carries the right scale — no bright flash.
  hdr_start_output_read(self);
  for (int guard = 0;
       (self->pending_desc != nullptr || self->pending_info != nullptr) &&
       guard < 8;
       guard++) {
    wl_display_roundtrip_queue(self->display, self->queue);
  }
  gint64 t_output_read = g_get_monotonic_time();
  if (self->out_max_lum == 0 || self->out_ref_lum == 0) {
    g_warning(
        "FlCompositorHDR: could not read output luminances; using SDR-neutral "
        "defaults");
    self->out_min_lum = 2000;  // 0.2 cd/m2
    self->out_max_lum = 203;
    self->out_ref_lum = 203;
    hdr_set_present_scale(self, 1.0f);
  }

  self->surface = wl_compositor_create_surface(self->comp);
  wl_surface_add_listener(self->surface, &surface_listener, self);
  self->subsurface = wl_subcompositor_get_subsurface(
      self->subcomp, self->surface, self->parent);
  // Sync mode: the subsurface's committed content applies ATOMICALLY with the
  // parent's commit, so the transparent-cleared parent never shows a frame
  // without our content. render() (main thread) commits the subsurface right
  // before GDK commits the parent, so the two latch together.
  wl_subsurface_set_sync(self->subsurface);
  struct wl_region* empty = wl_compositor_create_region(self->comp);
  wl_surface_set_input_region(self->surface, empty);
  wl_region_destroy(empty);
  if (self->viewporter) {
    self->viewport =
        wp_viewporter_get_viewport(self->viewporter, self->surface);
  }

  // Tag the surface extended-linear (sRGB primaries) at the output's own
  // luminance volume, blocking until the description is ready so the first
  // commit already carries it. KWin anchors extended-linear 1.0 at max_lum,
  // so pixels are pre-scaled by reference/max in the present draw: SDR white
  // (Skia linear 1.0) lands at reference nits, HDR highlights reach up to
  // max nits, brighter clamps at the panel peak.
  self->cm_surface = wp_color_manager_v1_get_surface(self->cm, self->surface);
  hdr_retag_surface(self);
  for (int guard = 0; self->surface_desc_pending != nullptr && guard < 8;
       guard++) {
    wl_display_roundtrip_queue(self->display, self->queue);
  }
  if (self->surface_desc == nullptr) {
    g_warning("FlCompositorHDR: extended-linear image description not ready");
    return FALSE;
  }
  gint64 t_tag = g_get_monotonic_time();
  // One-time cost, before the window maps. Measured 1.2-2.0ms total on
  // KWin 6.6 (registry / caps / output-read / surface-tag each <1.5ms).
  g_debug(
      "FlCompositorHDR: setup blocking roundtrips %.2fms total "
      "(registry %.2f, caps %.2f, output-read %.2f, surface+tag %.2f)",
      (t_tag - t0) / 1000.0, (t_registry - t0) / 1000.0,
      (t_caps - t_registry) / 1000.0, (t_output_read - t_caps) / 1000.0,
      (t_tag - t_output_read) / 1000.0);
  g_debug(
      "FlCompositorHDR: F16 extended-linear present active (min=%.4f max=%u "
      "ref=%u cd/m2, headroom %.2fx)",
      self->out_min_lum / 1e4, self->out_max_lum, self->out_ref_lum,
      (double)self->out_max_lum / self->out_ref_lum);

  wl_display_flush(self->display);
  return TRUE;
}

// ---------------------------------------------------------------------------
static void fl_compositor_hdr_dispose(GObject* object) {
  FlCompositorHDR* self = FL_COMPOSITOR_HDR(object);
  g_mutex_lock(&self->mutex);
  self->disposed = TRUE;
  g_mutex_unlock(&self->mutex);

  // Remove the queue source before the queue it dispatches is destroyed
  // (the source and dispose both run on the main thread, so no callback is
  // in flight).
  if (self->queue_source_id != 0) {
    g_source_remove(self->queue_source_id);
    self->queue_source_id = 0;
  }

  // Trusting make_current alone is not enough: on a window already being
  // destroyed it can report success without a context actually current, and
  // epoxy's gl* dispatch abort()s on the first call. eglGetCurrentContext is
  // core EGL, safe to call with no context current.
  gboolean have_gl = self->opengl_manager != nullptr &&
                     fl_opengl_manager_make_current(self->opengl_manager) &&
                     eglGetCurrentContext() != EGL_NO_CONTEXT;
  for (int i = 0; i < HDR_RING; i++) {
    hdr_buffer_free(self->ring[i], have_gl);
    self->ring[i] = nullptr;
  }
  for (GList* l = self->retired; l; l = l->next) {
    hdr_buffer_free(static_cast<HdrBuffer*>(l->data), have_gl);
  }
  if (have_gl) {
    if (self->program != 0) {
      glDeleteProgram(self->program);
      self->program = 0;
    }
    if (self->vertex_buffer != 0) {
      glDeleteBuffers(1, &self->vertex_buffer);
      self->vertex_buffer = 0;
    }
  }
  g_clear_pointer(&self->retired, g_list_free);

  if (self->pending_info) {
    wp_image_description_info_v1_destroy(self->pending_info);
    self->pending_info = nullptr;
  }
  if (self->pending_desc) {
    wp_image_description_v1_destroy(self->pending_desc);
    self->pending_desc = nullptr;
  }
  if (self->surface_desc_pending) {
    wp_image_description_v1_destroy(self->surface_desc_pending);
    self->surface_desc_pending = nullptr;
  }
  if (self->surface_desc) {
    wp_image_description_v1_destroy(self->surface_desc);
    self->surface_desc = nullptr;
  }
  if (self->cm_surface) {
    wp_color_management_surface_v1_destroy(self->cm_surface);
    self->cm_surface = nullptr;
  }
  g_clear_pointer(&self->outputs, g_ptr_array_unref);
  self->current_output = nullptr;

  if (self->viewport) {
    wp_viewport_destroy(self->viewport);
    self->viewport = nullptr;
  }
  if (self->subsurface) {
    wl_subsurface_destroy(self->subsurface);
    self->subsurface = nullptr;
  }
  if (self->surface) {
    wl_surface_destroy(self->surface);
    self->surface = nullptr;
  }
  g_clear_pointer(&self->viewporter, wp_viewporter_destroy);
  g_clear_pointer(&self->subcomp, wl_subcompositor_destroy);
  g_clear_pointer(&self->dmabuf, zwp_linux_dmabuf_v1_destroy);
  g_clear_pointer(&self->cm, wp_color_manager_v1_destroy);
  g_clear_pointer(&self->comp, wl_compositor_destroy);
  if (self->registry) {
    wl_registry_destroy(self->registry);
    self->registry = nullptr;
  }
  if (self->queue) {
    wl_event_queue_destroy(self->queue);
    self->queue = nullptr;
  }
  if (self->gbm) {
    gbm_device_destroy(self->gbm);
    self->gbm = nullptr;
  }
  if (self->drm_fd >= 0) {
    close(self->drm_fd);
    self->drm_fd = -1;
  }
  g_clear_object(&self->task_runner);
  g_clear_object(&self->opengl_manager);
  g_mutex_clear(&self->mutex);
  g_mutex_clear(&self->dispatch_mutex);

  G_OBJECT_CLASS(fl_compositor_hdr_parent_class)->dispose(object);
}

static void fl_compositor_hdr_class_init(FlCompositorHDRClass* klass) {
  FL_COMPOSITOR_CLASS(klass)->present_layers = fl_compositor_hdr_present_layers;
  FL_COMPOSITOR_CLASS(klass)->render = fl_compositor_hdr_render;
  G_OBJECT_CLASS(klass)->dispose = fl_compositor_hdr_dispose;

  // Emitted on the main thread when the display EDR headroom changes (the
  // output's image description delivered new luminances — HDR toggled,
  // window moved to a different output, compositor retargeted). Read the
  // new value with fl_compositor_hdr_get_display_headroom.
  fl_compositor_hdr_signals[SIGNAL_HEADROOM_CHANGED] = g_signal_new(
      "headroom-changed", fl_compositor_hdr_get_type(), G_SIGNAL_RUN_LAST, 0,
      nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
}

static void fl_compositor_hdr_init(FlCompositorHDR* self) {
  g_mutex_init(&self->mutex);
  g_mutex_init(&self->dispatch_mutex);
  self->drm_fd = -1;
  self->pending_slot = -1;
  hdr_set_present_scale(self, 1.0f);
  self->headroom_milli = 1000;  // 1.0x = SDR until the output is read
  self->outputs = g_ptr_array_new_with_free_func(hdr_output_free);
}

double fl_compositor_hdr_get_display_headroom(FlCompositorHDR* self) {
  g_return_val_if_fail(FL_IS_COMPOSITOR_HDR(self), 1.0);
  return g_atomic_int_get(&self->headroom_milli) / 1000.0;
}

// Main-loop dispatch of the private queue while no frames present. Events
// (an output's image description changing when HDR is toggled, buffer
// releases) land on the queue whenever the display socket is read — GDK's
// event source polls that fd and reads continuously — but they sit there
// until DISPATCHED, which otherwise only happens inside present_layers: a
// fully idle app would never see an ambient headroom change.
//
// This source is event-driven with no fd of its own: the queue's only
// producer is a socket read, and every socket read is preceded by a
// main-loop wake (GDK's poll on the display fd), so probing the queue at the
// top of each loop iteration cannot miss — events cannot arrive without a
// wake. The probe is wl_display_prepare_read_queue used purely as an
// emptiness test: nonzero means events are pending (dispatch); zero means
// the queue is empty and we were registered as a socket reader, which we
// cancel before returning. The register + cancel both happen inside
// prepare() — holding reader registration across the poll would deadlock
// GDK's wl_display_read_events, which waits for ALL registered readers.
typedef struct {
  GSource parent;
  FlCompositorHDR* self;  // borrowed; the source is removed in dispose
} HdrQueueSource;

static gboolean hdr_queue_source_prepare(GSource* source, gint* timeout) {
  HdrQueueSource* s = reinterpret_cast<HdrQueueSource*>(source);
  *timeout = -1;
  if (wl_display_prepare_read_queue(s->self->display, s->self->queue) != 0) {
    return TRUE;  // events pending on our queue
  }
  wl_display_cancel_read(s->self->display);
  return FALSE;
}

static gboolean hdr_queue_source_check(GSource* source) {
  return FALSE;  // no fds; prepare() is the only trigger
}

static gboolean hdr_queue_source_dispatch(GSource* source,
                                          GSourceFunc callback,
                                          gpointer user_data) {
  HdrQueueSource* s = reinterpret_cast<HdrQueueSource*>(source);
  FlCompositorHDR* self = s->self;
  g_mutex_lock(&self->dispatch_mutex);
  wl_display_dispatch_queue_pending(self->display, self->queue);
  g_mutex_unlock(&self->dispatch_mutex);
  return G_SOURCE_CONTINUE;
}

static GSourceFuncs hdr_queue_source_funcs = {
    hdr_queue_source_prepare,
    hdr_queue_source_check,
    hdr_queue_source_dispatch,
    nullptr,
    nullptr,
    nullptr,
};

FlCompositorHDR* fl_compositor_hdr_new(FlTaskRunner* task_runner,
                                       FlOpenGLManager* opengl_manager,
                                       GtkWidget* widget) {
  // Not viable outside Wayland; caller falls back to FlCompositorOpenGL.
  if (!GDK_IS_WAYLAND_DISPLAY(gtk_widget_get_display(widget))) {
    return nullptr;
  }
  FlCompositorHDR* self =
      FL_COMPOSITOR_HDR(g_object_new(fl_compositor_hdr_get_type(), nullptr));
  self->task_runner = FL_TASK_RUNNER(g_object_ref(task_runner));
  self->opengl_manager = FL_OPENGL_MANAGER(g_object_ref(opengl_manager));
  self->widget = widget;
  self->wl_ok = hdr_setup(self);
  if (!self->wl_ok) {
    g_object_unref(self);
    return nullptr;
  }
  GSource* queue_source =
      g_source_new(&hdr_queue_source_funcs, sizeof(HdrQueueSource));
  reinterpret_cast<HdrQueueSource*>(queue_source)->self = self;
  g_source_set_name(queue_source, "FlCompositorHDR Wayland queue");
  self->queue_source_id = g_source_attach(queue_source, nullptr);
  g_source_unref(queue_source);
  return self;
}
