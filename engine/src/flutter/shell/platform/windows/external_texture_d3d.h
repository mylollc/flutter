// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_WINDOWS_EXTERNAL_TEXTURE_D3D_H_
#define FLUTTER_SHELL_PLATFORM_WINDOWS_EXTERNAL_TEXTURE_D3D_H_

#include <wrl/client.h>

#include <memory>

#include "flutter/fml/macros.h"
#include "flutter/shell/platform/common/public/flutter_texture_registrar.h"
#include "flutter/shell/platform/windows/egl/manager.h"
#include "flutter/shell/platform/windows/egl/proc_table.h"
#include "flutter/shell/platform/windows/external_texture.h"

struct ID3D11Texture2D;

namespace flutter {

// An external texture that is backed by a DXGI surface.
class ExternalTextureD3d : public ExternalTexture {
 public:
  ExternalTextureD3d(
      FlutterDesktopGpuSurfaceType type,
      const FlutterDesktopGpuSurfaceTextureCallback texture_callback,
      void* user_data,
      const egl::Manager* egl_manager,
      std::shared_ptr<egl::ProcTable> gl);
  virtual ~ExternalTextureD3d();

  // |ExternalTexture|
  bool PopulateTexture(size_t width,
                       size_t height,
                       FlutterOpenGLTexture* opengl_texture) override;

 private:
  // Creates or updates the backing texture and associates it with the provided
  // surface.
  bool CreateOrUpdateTexture(
      const FlutterDesktopGpuSurfaceDescriptor* descriptor);
  // Detaches the previously attached surface, if any.
  void ReleaseImage();

  FlutterDesktopGpuSurfaceType type_;
  const FlutterDesktopGpuSurfaceTextureCallback texture_callback_;
  void* const user_data_;
  const egl::Manager* egl_manager_;
  std::shared_ptr<egl::ProcTable> gl_;
  GLuint gl_texture_ = 0;
  EGLSurface egl_surface_ = EGL_NO_SURFACE;
  void* last_surface_handle_ = nullptr;
  // True when the D3D texture is DXGI_FORMAT_R16G16B16A16_FLOAT.
  // Used to report GL_RGBA16F_EXT instead of GL_RGBA8_OES to the engine,
  // preserving HDR values >1.0.
  bool is_rgba16float_ = false;
  // For FP16 shared-handle textures: ANGLE's pbuffer-from-share-handle path
  // (EGL_D3D_TEXTURE_2D_SHARE_HANDLE_ANGLE) rejects FP16. We keep the
  // ANGLE-device-local copy alive and use EGL_D3D_TEXTURE_ANGLE (direct
  // pointer) against it, which supports FP16. Null for 8-bit or when the
  // surface came through the direct-pointer path originally.
  Microsoft::WRL::ComPtr<ID3D11Texture2D> angle_device_texture_;

  FML_DISALLOW_COPY_AND_ASSIGN(ExternalTextureD3d);
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_WINDOWS_EXTERNAL_TEXTURE_D3D_H_
