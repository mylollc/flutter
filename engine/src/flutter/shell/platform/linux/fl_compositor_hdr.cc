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
#include "flutter/shell/platform/linux/wayland_vendor/linux-dmabuf-v1-client-protocol.h"
#include "flutter/shell/platform/linux/wayland_vendor/viewporter-client-protocol.h"

// DRM fourccs (avoid a libdrm include; these are the stable fourcc codes).
#define OLYM_FOURCC(a, b, c, d)                                    \
  ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | \
   ((uint32_t)(d) << 24))
#define OLYM_DRM_FORMAT_ABGR8888 OLYM_FOURCC('A', 'B', '2', '4')

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

// Present ring depth. MUST stay at 2 on this stack: iris/Arrow-Lake silently
// stops honoring glBlitFramebuffer into the 3rd+ concurrent LINEAR dma-buf
// render target (GL reports success, but the write never reaches memory —
// glClear into the same FBO DOES land, so it is a blit-path limit, not the
// target). With Flutter's own shareable backing-store FBOs also live, only our
// first two ring slots ever receive blits; a 3rd slot would show stale content
// (the loading-screen flicker). Two slots = classic double-buffer; under
// compositor backpressure a frame is dropped (repeats the last good frame),
// never blanked. Deeper buffering needs a shader-draw present path (draws land
// where blits don't) — that is also what HDR step 3 tone-mapping requires.
#define HDR_RING 2

// A single dma-buf slot: a GBM bo rendered into via an EGLImage-backed FBO, and
// presented as the same bo's wl_buffer. Heap-allocated so it survives a resize
// reallocation (moved to the `retired` list, freed on release) without
// invalidating its wl_buffer.release listener data.
typedef struct _HdrBuffer {
  struct _FlCompositorHDR* owner;
  struct gbm_bo* bo;
  EGLImageKHR image;
  GLuint texture;
  GLuint fbo;
  struct wl_buffer* buffer;
  size_t w, h;
  gboolean busy;     // committed; the compositor holds it until release
  gboolean retired;  // dropped from the ring on resize; free on release
} HdrBuffer;

struct _FlCompositorHDR {
  FlCompositor parent_instance;

  FlTaskRunner* task_runner;
  FlOpenGLManager* opengl_manager;
  GtkWidget* widget;  // FlView render widget (not owned)

  // Wayland — created on the main thread at construction.
  struct wl_display* display;
  struct wl_event_queue* queue;
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

  struct gbm_device* gbm;
  int drm_fd;

  // Ring lives entirely on the raster thread (present_layers, its queue-drain
  // release handling, and the retired sweep). render() commits the pending slot.
  HdrBuffer* ring[HDR_RING];
  GList* retired;  // HdrBuffer* awaiting release after a resize
  size_t ring_w, ring_h;

  // Present handoff. present_layers (raster) does the GL blit into a ring slot
  // and publishes `pending_slot`; render() (main thread) does the Wayland
  // attach/commit of that slot 1:1 with the parent surface's commit, so the two
  // surfaces latch atomically. `busy` is written by render() (commit) and the
  // release callback, read by present_layers (slot pick) — all under `mutex`.
  GMutex mutex;
  int pending_slot;  // -1 = nothing waiting; else ring index blitted & ready
};

G_DEFINE_TYPE(FlCompositorHDR, fl_compositor_hdr, fl_compositor_get_type())

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
  }
}
static void registry_global_remove(void* data,
                                   struct wl_registry* reg,
                                   uint32_t name) {}
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
// buffer lifecycle
// ---------------------------------------------------------------------------
static void hdr_buffer_free(HdrBuffer* b) {
  if (!b) {
    return;
  }
  if (b->fbo) {
    glDeleteFramebuffers(1, &b->fbo);
  }
  if (b->texture) {
    glDeleteTextures(1, &b->texture);
  }
  if (b->image != EGL_NO_IMAGE_KHR) {
    eglDestroyImageKHR(eglGetCurrentDisplay(), b->image);
  }
  if (b->buffer) {
    wl_buffer_destroy(b->buffer);
  }
  if (b->bo) {
    gbm_bo_destroy(b->bo);
  }
  free(b);
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
      hdr_buffer_free(b);  // GL deletes on the raster context (this thread)
    }
  }
  g_list_free(self->retired);
  self->retired = keep;
}

static HdrBuffer* hdr_buffer_new(FlCompositorHDR* self, size_t w, size_t h) {
  HdrBuffer* b = static_cast<HdrBuffer*>(calloc(1, sizeof(HdrBuffer)));
  b->owner = self;
  b->image = EGL_NO_IMAGE_KHR;
  b->w = w;
  b->h = h;

  b->bo = gbm_bo_create(self->gbm, w, h, OLYM_DRM_FORMAT_ABGR8888,
                        GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
  if (!b->bo) {
    g_warning("FlCompositorHDR: gbm_bo_create %zux%zu failed", w, h);
    hdr_buffer_free(b);
    return nullptr;
  }
  uint32_t stride = gbm_bo_get_stride(b->bo);
  uint32_t offset = gbm_bo_get_offset(b->bo, 0);

  EGLDisplay dpy = eglGetCurrentDisplay();
  int fd_egl = gbm_bo_get_fd(b->bo);
  EGLint attrs[] = {EGL_WIDTH,
                    (EGLint)w,
                    EGL_HEIGHT,
                    (EGLint)h,
                    EGL_LINUX_DRM_FOURCC_EXT,
                    (EGLint)OLYM_DRM_FORMAT_ABGR8888,
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
    g_warning("FlCompositorHDR: eglCreateImageKHR failed egl=0x%x",
              eglGetError());
    hdr_buffer_free(b);
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
    g_warning("FlCompositorHDR: dma-buf FBO incomplete");
    hdr_buffer_free(b);
    return nullptr;
  }

  int fd_wl = gbm_bo_get_fd(b->bo);
  struct zwp_linux_buffer_params_v1* params =
      zwp_linux_dmabuf_v1_create_params(self->dmabuf);
  zwp_linux_buffer_params_v1_add(params, fd_wl, 0, offset, stride, 0, 0);
  b->buffer = zwp_linux_buffer_params_v1_create_immed(
      params, w, h, OLYM_DRM_FORMAT_ABGR8888, 0);
  zwp_linux_buffer_params_v1_destroy(params);
  close(fd_wl);
  if (!b->buffer) {
    g_warning("FlCompositorHDR: zwp_linux_buffer_params create_immed failed");
    hdr_buffer_free(b);
    return nullptr;
  }
  wl_buffer_add_listener(b->buffer, &buffer_listener, b);
  return b;
}

// ---------------------------------------------------------------------------
// GBM device on the GL context's own render node (raster thread) — no
// cross-GPU write; KWin imports the result for the display (standard PRIME).
// ---------------------------------------------------------------------------
static gboolean hdr_gbm_ensure(FlCompositorHDR* self) {
  if (self->gbm) {
    return TRUE;
  }
  const char* node = nullptr;
  EGLDisplay dpy = eglGetCurrentDisplay();
  if (dpy != EGL_NO_DISPLAY &&
      epoxy_has_egl_extension(dpy, "EGL_EXT_device_query")) {
    EGLAttrib dev = 0;
    if (eglQueryDisplayAttribEXT(dpy, EGL_DEVICE_EXT, &dev) && dev) {
      node = eglQueryDeviceStringEXT((EGLDeviceEXT)dev,
                                     EGL_DRM_RENDER_NODE_FILE_EXT);
      if (!node) {
        node = eglQueryDeviceStringEXT((EGLDeviceEXT)dev,
                                       EGL_DRM_DEVICE_FILE_EXT);
      }
    }
  }
  if (!node) {
    node = "/dev/dri/renderD128";  // Intel/Mesa fallback (GBM-capable, eDP GPU)
  }
  self->drm_fd = open(node, O_RDWR | O_CLOEXEC);
  if (self->drm_fd < 0) {
    g_warning("FlCompositorHDR: open(%s) failed", node);
    return FALSE;
  }
  self->gbm = gbm_create_device(self->drm_fd);
  if (!self->gbm) {
    g_warning("FlCompositorHDR: gbm_create_device(%s) failed", node);
    return FALSE;
  }
  return TRUE;
}

// (Re)allocate the ring for size w x h (raster thread, GL context current).
// On a size change, in-flight (busy) buffers are RETIRED (kept alive until
// their release) rather than freed, so the main thread can safely still be
// presenting one; non-busy buffers are freed immediately.
static gboolean hdr_ring_ensure(FlCompositorHDR* self, size_t w, size_t h) {
  if (!hdr_gbm_ensure(self)) {
    return FALSE;
  }
  if (self->ring_w == w && self->ring_h == h && self->ring[0]) {
    return TRUE;
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
      hdr_buffer_free(b);
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
  self->ring_w = w;
  self->ring_h = h;
  return TRUE;
}

// ---------------------------------------------------------------------------
// FlCompositor::present_layers (raster thread) — blit the layer into a free
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
  // We present only the base backing store (layers[0]); Olym's Linux scene is a
  // single backing-store layer (no platform views).
  const FlutterLayer* layer = layers[0];
  if (layer->type != kFlutterLayerContentTypeBackingStore) {
    return TRUE;
  }
  FlFramebuffer* src =
      FL_FRAMEBUFFER(layer->backing_store->open_gl.framebuffer.user_data);
  size_t w = layer->size.width;
  size_t h = layer->size.height;

  wl_display_dispatch_queue_pending(self->display, self->queue);  // releases
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

  // Composite: blit the backing store into the bo-backed FBO, flipping Y (GL
  // origin is bottom-left; the Wayland buffer wants the top row first).
  GLint saved_read = 0, saved_draw = 0;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &saved_read);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &saved_draw);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fl_framebuffer_get_id(src));
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, b->fbo);
  // Skia hands off on the raster thread with GL_SCISSOR_TEST left ENABLED around
  // its last clipped draw. glBlitFramebuffer is subject to the scissor test, so
  // that leftover rectangle clips our present blit — only the scissored sub-rect
  // of the slot gets the new frame and the rest keeps stale content. On the
  // loading screen everything is drawn inside small ClipRRects => a small
  // scissor => most of the slot is stale => the flicker (binary blink + the
  // horizontal streaks at the scissor edges). Steady-state (full-viewport
  // repaint) leaves scissor full/disabled, which is why it looked fine. Stock
  // FlCompositorOpenGL disables scissor around its blit for the same reason
  // (flutter#140828). Save/restore so Skia's state is untouched.
  GLboolean saved_scissor = glIsEnabled(GL_SCISSOR_TEST);
  glDisable(GL_SCISSOR_TEST);
  glBlitFramebuffer(0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  if (saved_scissor) {
    glEnable(GL_SCISSOR_TEST);
  }
  glBindFramebuffer(GL_READ_FRAMEBUFFER, saved_read);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, saved_draw);
  // Wait for the blit to actually COMPLETE before handing the dma-buf to the
  // compositor — glFlush only submits, so the compositor could sample a
  // half-written buffer. glFinish is the simple correct barrier; an EGL fence
  // (glClientWaitSync / explicit sync) is the later optimization.
  glFinish();

  // Hand the blitted slot to render(): it does the wl_surface attach/commit on
  // the main thread, exactly once per parent-surface commit, so the subsurface
  // and parent latch atomically. If a prior pending slot was never committed
  // (render() hasn't run yet), it is simply superseded here — we always present
  // the newest frame.
  g_mutex_lock(&self->mutex);
  self->pending_slot = slot;
  g_mutex_unlock(&self->mutex);
  return TRUE;
}

// ---------------------------------------------------------------------------
// FlCompositor::render (main thread) — commit the slot present_layers blitted
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

  // Commit the slot present_layers blitted, here on the main thread — exactly
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
      wp_viewport_set_destination(self->viewport, alloc.width, alloc.height);
    } else {
      wl_surface_set_buffer_scale(self->surface, scale > 0 ? scale : 1);
    }
    wl_surface_attach(self->surface, b->buffer, 0, 0);
    wl_surface_damage_buffer(self->surface, 0, 0, b->w, b->h);
    wl_surface_commit(self->surface);
    wl_display_flush(self->display);
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

  self->queue = wl_display_create_queue(self->display);
  struct wl_registry* reg = wl_display_get_registry(self->display);
  wl_proxy_set_queue((struct wl_proxy*)reg, self->queue);
  wl_registry_add_listener(reg, &registry_listener, self);
  wl_display_roundtrip_queue(self->display, self->queue);
  if (!self->comp || !self->subcomp || !self->dmabuf) {
    g_warning("FlCompositorHDR: required Wayland globals missing "
              "(wl_compositor/wl_subcompositor/zwp_linux_dmabuf_v1)");
    wl_registry_destroy(reg);
    return FALSE;
  }
  zwp_linux_dmabuf_v1_add_listener(self->dmabuf, &dmabuf_listener, self);
  wl_registry_destroy(reg);

  self->surface = wl_compositor_create_surface(self->comp);
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
  wl_display_flush(self->display);
  return TRUE;
}

// ---------------------------------------------------------------------------
static void fl_compositor_hdr_dispose(GObject* object) {
  FlCompositorHDR* self = FL_COMPOSITOR_HDR(object);
  g_mutex_lock(&self->mutex);
  self->disposed = TRUE;
  g_mutex_unlock(&self->mutex);

  gboolean have_gl =
      self->opengl_manager != nullptr &&
      fl_opengl_manager_make_current(self->opengl_manager);
  for (int i = 0; i < HDR_RING; i++) {
    if (have_gl) {
      hdr_buffer_free(self->ring[i]);
    }
    self->ring[i] = nullptr;
  }
  if (have_gl) {
    for (GList* l = self->retired; l; l = l->next) {
      hdr_buffer_free(static_cast<HdrBuffer*>(l->data));
    }
  }
  g_clear_pointer(&self->retired, g_list_free);

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
  g_clear_pointer(&self->comp, wl_compositor_destroy);
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

  G_OBJECT_CLASS(fl_compositor_hdr_parent_class)->dispose(object);
}

static void fl_compositor_hdr_class_init(FlCompositorHDRClass* klass) {
  FL_COMPOSITOR_CLASS(klass)->present_layers = fl_compositor_hdr_present_layers;
  FL_COMPOSITOR_CLASS(klass)->render = fl_compositor_hdr_render;
  G_OBJECT_CLASS(klass)->dispose = fl_compositor_hdr_dispose;
}

static void fl_compositor_hdr_init(FlCompositorHDR* self) {
  g_mutex_init(&self->mutex);
  self->drm_fd = -1;
  self->pending_slot = -1;
}

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
  return self;
}
