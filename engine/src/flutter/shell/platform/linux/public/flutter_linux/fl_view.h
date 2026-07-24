// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_LINUX_PUBLIC_FLUTTER_LINUX_FL_VIEW_H_
#define FLUTTER_SHELL_PLATFORM_LINUX_PUBLIC_FLUTTER_LINUX_FL_VIEW_H_

#if !defined(__FLUTTER_LINUX_INSIDE__) && !defined(FLUTTER_LINUX_COMPILATION)
#error "Only <flutter_linux/flutter_linux.h> can be included directly."
#endif

#include <gmodule.h>
#include <gtk/gtk.h>

#include "fl_dart_project.h"
#include "fl_engine.h"

G_BEGIN_DECLS

G_MODULE_EXPORT
G_DECLARE_FINAL_TYPE(FlView, fl_view, FL, VIEW, GtkBox)

/**
 * FlView:
 *
 * #FlView is a GTK widget that is capable of displaying a Flutter application.
 *
 * The following example shows how to set up a view in a GTK application:
 * |[<!-- language="C" -->
 *   FlDartProject *project = fl_dart_project_new ();
 *   FlView *view = fl_view_new (project);
 *   gtk_widget_show (GTK_WIDGET (view));
 *   gtk_container_add (GTK_CONTAINER (parent), view);
 *
 *   FlBinaryMessenger *messenger =
 *     fl_engine_get_binary_messenger (fl_view_get_engine (view));
 *   setup_channels_or_plugins (messenger);
 * ]|
 */

/**
 * fl_view_new:
 * @project: The project to show.
 *
 * Creates a widget to show a Flutter application.
 *
 * Returns: a new #FlView.
 */
FlView* fl_view_new(FlDartProject* project);

/**
 * fl_view_new_for_engine:
 * @engine: an #FlEngine.
 *
 * Creates a widget to show a window in a Flutter application.
 * The engine must be not be headless.
 *
 * Returns: a new #FlView.
 */
FlView* fl_view_new_for_engine(FlEngine* engine);

/**
 * fl_view_get_engine:
 * @view: an #FlView.
 *
 * Gets the engine being rendered in the view.
 *
 * Returns: an #FlEngine.
 */
FlEngine* fl_view_get_engine(FlView* view);

/**
 * fl_view_get_id:
 * @view: an #FlView.
 *
 * Gets the Flutter view ID used by this view.
 *
 * Returns: a view ID or -1 if now ID assigned.
 */
int64_t fl_view_get_id(FlView* view);

/**
 * fl_view_set_background_color:
 * @view: an #FlView.
 * @color: a background color.
 *
 * Set the background color for Flutter (defaults to black).
 */
void fl_view_set_background_color(FlView* view, const GdkRGBA* color);

/**
 * fl_view_set_hdr_enabled:
 * @view: an #FlView.
 * @enable: %TRUE to request HDR presentation.
 *
 * Requests HDR presentation for this view. Must be called before the view is
 * realized. When enabled and the platform can provide it (a Wayland session
 * whose compositor supports the color-management protocol), the view renders
 * into linear half-float (scRGB) surfaces presented with extended dynamic
 * range: 1.0 stays SDR white and values above it use the display's HDR
 * headroom (see fl_view_get_display_headroom). When the platform cannot
 * provide it, presentation falls back to the standard SDR path.
 *
 * Off by default: HDR presentation doubles the view's backing-store memory
 * (half-float instead of 8-bit), which only pays off for applications that
 * render HDR content.
 */
void fl_view_set_hdr_enabled(FlView* view, gboolean enable);

/**
 * fl_view_get_display_headroom:
 * @view: an #FlView.
 *
 * Gets the current EDR headroom of the display the view is on: the output's
 * maximum luminance divided by its reference (SDR white) luminance, read live
 * from the Wayland color-management protocol. 1.0 when the display (or the
 * session's presentation path) is SDR-only. Plugins rendering HDR content can
 * read this to drive tone mapping — the Linux analog of macOS's
 * NSScreen.maximumExtendedDynamicRangeColorComponentValue. Thread-safe. The
 * view emits "display-headroom-changed" (no arguments, main thread) when the
 * value changes, so consumers can re-render instead of polling.
 *
 * Returns: the headroom multiplier (>= 1.0).
 */
double fl_view_get_display_headroom(FlView* view);

G_END_DECLS

#endif  // FLUTTER_SHELL_PLATFORM_LINUX_PUBLIC_FLUTTER_LINUX_FL_VIEW_H_
