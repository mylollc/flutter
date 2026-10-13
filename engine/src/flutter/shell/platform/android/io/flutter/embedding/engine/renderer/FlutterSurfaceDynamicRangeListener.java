// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package io.flutter.embedding.engine.renderer;

/**
 * Listener invoked when the dynamic range of the surface Flutter renders into changes, such as when
 * the surface is recreated.
 */
public interface FlutterSurfaceDynamicRangeListener {
  /**
   * The surface Flutter renders into changed dynamic range.
   *
   * @param extendedRange {@code true} when the surface stores extended-range (F16) color, so
   *     content brighter than SDR white reaches the display; {@code false} when it is an SDR
   *     surface that clips such content, as on the OpenGL ES backend or a device without an F16
   *     swapchain.
   */
  void onSurfaceDynamicRangeChanged(boolean extendedRange);
}
