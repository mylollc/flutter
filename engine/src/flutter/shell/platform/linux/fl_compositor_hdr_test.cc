// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux/fl_compositor_hdr.h"

#include <gdk/gdk.h>
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif

#include "flutter/shell/platform/linux/fl_engine_private.h"
#include "flutter/shell/platform/linux/fl_opengl_manager.h"
#include "flutter/shell/platform/linux/fl_task_runner.h"
#include "flutter/shell/platform/linux/public/flutter_linux/fl_dart_project.h"
#include "flutter/shell/platform/linux/testing/fl_test_gtk_logs.h"
#include "gtest/gtest.h"

// FlCompositorHDR requires a live Wayland session with color management to
// fully construct (fl_compositor_hdr_new), which the test environment does
// not provide. These tests cover the surface that must hold WITHOUT one:
// the factory's clean refusal, the SDR defaults consumers divide by, the
// signal contract, and that a bare (never set up) instance disposes safely
// — the state every construction-failure path leaves behind.

// The factory returns NULL (and leaks nothing) when the display is not
// Wayland; fl_view then falls back to FlCompositorOpenGL.
TEST(FlCompositorHdrTest, NewReturnsNullWithoutWayland) {
  flutter::testing::fl_ensure_gtk_init();

#ifdef GDK_WINDOWING_WAYLAND
  if (GDK_IS_WAYLAND_DISPLAY(gdk_display_get_default())) {
    GTEST_SKIP() << "test environment is a live Wayland session";
  }
#endif

  g_autoptr(FlDartProject) project = fl_dart_project_new();
  g_autoptr(FlEngine) engine = fl_engine_new(project);
  g_autoptr(FlTaskRunner) task_runner = fl_task_runner_new(engine);
  g_autoptr(FlOpenGLManager) opengl_manager = fl_opengl_manager_new();
  GtkWidget* widget = gtk_label_new("");
  g_object_ref_sink(widget);

  FlCompositorHDR* compositor =
      fl_compositor_hdr_new(task_runner, opengl_manager, widget);
  EXPECT_EQ(compositor, nullptr);

  g_object_unref(widget);
}

// Until (unless) an output image description is read, the headroom is
// exactly 1.0 — SDR. Application code divides by this value.
TEST(FlCompositorHdrTest, HeadroomDefaultsToSdr) {
  g_autoptr(FlCompositorHDR) compositor =
      FL_COMPOSITOR_HDR(g_object_new(fl_compositor_hdr_get_type(), nullptr));

  EXPECT_EQ(fl_compositor_hdr_get_display_headroom(compositor), 1.0);
}

// The signal FlView relays as "display-headroom-changed" is registered on
// the type.
TEST(FlCompositorHdrTest, HeadroomChangedSignalRegistered) {
  g_autoptr(FlCompositorHDR) compositor =
      FL_COMPOSITOR_HDR(g_object_new(fl_compositor_hdr_get_type(), nullptr));

  EXPECT_NE(g_signal_lookup("headroom-changed", G_OBJECT_TYPE(compositor)), 0u);
}

// present_layers is a safe no-op before Wayland setup succeeded (wl_ok is
// FALSE) — the state a failed construction or the raster thread racing
// dispose can observe.
TEST(FlCompositorHdrTest, PresentLayersWithoutSetupIsSafe) {
  g_autoptr(FlCompositorHDR) compositor =
      FL_COMPOSITOR_HDR(g_object_new(fl_compositor_hdr_get_type(), nullptr));

  EXPECT_TRUE(
      fl_compositor_present_layers(FL_COMPOSITOR(compositor), nullptr, 0));
}

// Dispose of a bare instance — no GL context, no Wayland objects, no ring —
// must not crash. This is the exact path a construction failure takes
// (fl_compositor_hdr_new unrefs on setup failure), and the window-close
// teardown regression class: GL/EGL entry points must never be dispatched
// without a proven-current context.
TEST(FlCompositorHdrTest, DisposeWithoutSetupIsSafe) {
  FlCompositorHDR* compositor =
      FL_COMPOSITOR_HDR(g_object_new(fl_compositor_hdr_get_type(), nullptr));

  g_object_unref(compositor);
}
