// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_WINDOWS_COMPOSITOR_OPENGL_H_
#define FLUTTER_SHELL_PLATFORM_WINDOWS_COMPOSITOR_OPENGL_H_

#include <memory>

#include "flutter/fml/macros.h"
#include "flutter/impeller/renderer/backend/gles/proc_table_gles.h"
#include "flutter/shell/platform/embedder/embedder.h"
#include "flutter/shell/platform/windows/compositor.h"
#include "flutter/shell/platform/windows/flutter_windows_engine.h"

namespace flutter {

// Enables the Flutter engine to render content on Windows using OpenGL.
class CompositorOpenGL : public Compositor {
 public:
  CompositorOpenGL(FlutterWindowsEngine* engine,
                   impeller::ProcTableGLES::Resolver resolver,
                   bool enable_impeller);

  /// |Compositor|
  bool CreateBackingStore(const FlutterBackingStoreConfig& config,
                          FlutterBackingStore* result) override;

  /// |Compositor|
  bool CollectBackingStore(const FlutterBackingStore* store) override;

  /// |Compositor|
  bool Present(FlutterWindowsView* view,
               const FlutterLayer** layers,
               size_t layers_count) override;

 private:
  // The Flutter engine that manages the views to render.
  FlutterWindowsEngine* engine_;

 private:
  struct TextureFormat {
    // The format passed to the engine using `FlutterOpenGLFramebuffer.target`.
    uint32_t sized_format = 0;
    // The internal format passed to `glTexImage2D`. Matches `general_format`
    // for 8-bit (where ANGLE's GLES accepts unsized internal formats) but is
    // the sized `GL_RGBA16F` for F16 (where ES requires a sized internal
    // format paired with `GL_HALF_FLOAT`).
    uint32_t internal_format = 0;
    // The external format (pixel layout) passed to `glTexImage2D`.
    uint32_t general_format = 0;
    // The pixel data type passed to `glTexImage2D`. GL_UNSIGNED_BYTE for 8-bit,
    // GL_HALF_FLOAT for RGBA16F HDR-capable backing stores.
    uint32_t pixel_type = 0;
  };

  // The compositor initializes itself lazily once |CreateBackingStore| is
  // called. True if initialization completed successfully.
  bool is_initialized_ = false;

  // Function used to resolve GLES functions.
  impeller::ProcTableGLES::Resolver resolver_ = nullptr;

  // Table of resolved GLES functions. Null until the compositor is initialized.
  std::unique_ptr<impeller::ProcTableGLES> gl_ = nullptr;

  // The OpenGL texture format for backing stores. Invalid value until
  // the compositor is initialized.
  TextureFormat format_;

  // Whether the Impeller rendering backend is enabled.
  bool enable_impeller_ = false;

  // The present pass of the F16 path. Skia composites the UI into the F16
  // backing store in gamma-encoded sRGB (embedder.cc tags it sRGB on
  // Windows), so text and translucent fills blend exactly as on the 8-bit
  // surface. The window surface is scRGB, which is linear with 1.0 = 80 nits,
  // so presenting draws the backing store through a shader that applies the
  // sRGB EOTF (extended to values above 1.0 for HDR content) and scales by the
  // display's SDR white level / 80 nits: UI white lands at the same
  // brightness as every other app's white, and HDR highlights keep their
  // headroom above it. Zero until first used; the program failing to build
  // falls back to the plain blit.
  uint32_t present_program_ = 0;
  uint32_t present_vertex_buffer_ = 0;
  int32_t present_scale_location_ = -1;
  int32_t present_texture_location_ = -1;
  bool present_program_failed_ = false;

  // Not in impeller::ProcTableGLES; resolved with |resolver_| for the present
  // pass's save and restore of vertex attribute 0.
  void (*get_vertex_attrib_iv_)(uint32_t, uint32_t, int32_t*) = nullptr;
  void (*get_vertex_attrib_pointer_v_)(uint32_t, uint32_t, void**) = nullptr;

  // SDR white / 80 nits for the monitor the view was last presented on.
  // Re-read when the window moves to another monitor and at most once a
  // second otherwise, since the SDR content brightness slider sends no
  // window message.
  float sdr_white_scale_ = 1.0f;
  void* sdr_white_monitor_ = nullptr;
  uint64_t sdr_white_read_ms_ = 0;

  // Initialize the compositor. This must run on the raster thread.
  bool Initialize();

  // Builds |present_program_|. Returns false (and sets
  // |present_program_failed_|) when the shaders do not compile or link.
  bool EnsurePresentProgram();

  // SDR white level of |hwnd|'s monitor divided by 80 nits; 1.0 when Windows
  // reports none (HDR off, or a display without advanced colour).
  float SdrWhiteScale(HWND hwnd);

  // Clear the view's surface and removes any previously presented layers.
  bool Clear(FlutterWindowsView* view);

  FML_DISALLOW_COPY_AND_ASSIGN(CompositorOpenGL);
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_WINDOWS_COMPOSITOR_OPENGL_H_
