// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_LINUX_FL_COMPOSITOR_HDR_H_
#define FLUTTER_SHELL_PLATFORM_LINUX_FL_COMPOSITOR_HDR_H_

#include <gtk/gtk.h>

#include "flutter/shell/platform/embedder/embedder.h"
#include "flutter/shell/platform/linux/fl_compositor.h"
#include "flutter/shell/platform/linux/fl_opengl_manager.h"
#include "flutter/shell/platform/linux/fl_task_runner.h"

G_BEGIN_DECLS

// An FlCompositor that presents Flutter's composited frame on an engine-owned
// Wayland wl_subsurface (a sibling above GDK's toplevel surface) backed by a
// GPU-resident dma-buf ring — Flutter composites
// straight into F16 (linear scRGB) backing stores, a present shader draws them
// into a GBM-backed ABGR16161616F FBO, and the same buffer is handed to the
// compositor as an extended-linear (EXT_LINEAR + sRGB primaries) tagged
// wl_buffer, so there is NO GPU->CPU readback. The compositor anchors
// extended-linear 1.0 at the output's max luminance, so the present draw scales
// every pixel by reference/max: SDR white lands on the desktop white, HDR
// highlights get the (max/reference) headroom above it. The output's luminances
// are read from its wp_color_management_output_v1 image description and re-read
// live on image_description_changed / surface enter (HDR toggles,
// SDR-brightness slider, monitor moves). Present + all Wayland event handling
// run on the engine raster thread via a dedicated wl_event_queue; GDK's normal
// redraw commits the parent so the (sync-mode) subsurface latches with it.
// Selected by fl_view when the application requested HDR
// (fl_view_set_hdr_enabled), the session is Wayland, and wp_color_manager_v1
// (with parametric/set_luminances/ext_linear capabilities) is present;
// otherwise fl_view falls back to FlCompositorOpenGL and the (X11 / no-CM)
// 8-bit path is untouched.

G_DECLARE_FINAL_TYPE(FlCompositorHDR,
                     fl_compositor_hdr,
                     FL,
                     COMPOSITOR_HDR,
                     FlCompositor)

/**
 * fl_compositor_hdr_new:
 * @task_runner: an #FlTaskRunner.
 * @opengl_manager: an #FlOpenGLManager (the raster GL context).
 * @widget: the FlView render widget (realized), used to reach GDK's toplevel
 *   wl_surface / wl_display / scale / allocation offset.
 *
 * Creates a subsurface-presenting compositor. Returns NULL if the session is
 * not Wayland or the required Wayland globals are unavailable (caller should
 * then fall back to FlCompositorOpenGL).
 *
 * Returns: (transfer full) (nullable): a new #FlCompositorHDR or NULL.
 */
FlCompositorHDR* fl_compositor_hdr_new(FlTaskRunner* task_runner,
                                       FlOpenGLManager* opengl_manager,
                                       GtkWidget* widget);

/**
 * fl_compositor_hdr_get_display_headroom:
 * @compositor: an #FlCompositorHDR.
 *
 * Gets the current display EDR headroom (the output's max luminance divided
 * by its reference/SDR-white luminance) of the output the view is on, read
 * live from the Wayland color-management output image description. 1.0 on an
 * SDR output. Thread-safe.
 *
 * Returns: the headroom multiplier (>= 1.0).
 */
double fl_compositor_hdr_get_display_headroom(FlCompositorHDR* compositor);

G_END_DECLS

#endif  // FLUTTER_SHELL_PLATFORM_LINUX_FL_COMPOSITOR_HDR_H_
