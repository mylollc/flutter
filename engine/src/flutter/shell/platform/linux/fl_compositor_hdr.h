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

// OLYM Phase-6 Linux HDR. An FlCompositor that presents Flutter's composited
// frame on an engine-owned Wayland wl_subsurface (a sibling above GDK's toplevel
// surface) backed by a GPU-resident dma-buf ring — Flutter composites straight
// into a GBM-backed FBO and the same buffer is handed to the compositor, so
// there is NO GPU->CPU readback. Present + all Wayland calls run on the engine
// raster thread via a dedicated wl_event_queue; GDK's normal redraw commits the
// parent so the subsurface maps. Step 2 keeps buffers 8-bit ABGR8888 (untagged)
// to de-risk surface ownership / resize / scale / commit-threading orthogonally
// to color; step 3 flips the fourcc to F16 + tags the surface extended-linear
// via wp_color_manager. Selected by fl_view when the session is Wayland and the
// required globals are present; otherwise fl_view falls back to
// FlCompositorOpenGL and the (X11 / no-CM) path is untouched.

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

G_END_DECLS

#endif  // FLUTTER_SHELL_PLATFORM_LINUX_FL_COMPOSITOR_HDR_H_
