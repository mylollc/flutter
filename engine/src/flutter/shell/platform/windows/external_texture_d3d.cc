// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/windows/external_texture_d3d.h"

#include <d3d11.h>
#include <wrl/client.h>

#include "flutter/fml/logging.h"
#include "flutter/shell/platform/embedder/embedder_struct_macros.h"

// GL_RGBA16F_EXT is defined in GLES2/gl2ext.h (0x881A).
// Use it for RGBA16Float D3D textures to preserve HDR values >1.0.
#ifndef GL_RGBA16F_EXT
#define GL_RGBA16F_EXT 0x881A
#endif

namespace flutter {

ExternalTextureD3d::ExternalTextureD3d(
    FlutterDesktopGpuSurfaceType type,
    const FlutterDesktopGpuSurfaceTextureCallback texture_callback,
    void* user_data,
    const egl::Manager* egl_manager,
    std::shared_ptr<egl::ProcTable> gl)
    : type_(type),
      texture_callback_(texture_callback),
      user_data_(user_data),
      egl_manager_(egl_manager),
      gl_(std::move(gl)) {}

ExternalTextureD3d::~ExternalTextureD3d() {
  ReleaseImage();

  if (gl_texture_ != 0) {
    gl_->DeleteTextures(1, &gl_texture_);
  }
}

bool ExternalTextureD3d::PopulateTexture(size_t width,
                                         size_t height,
                                         FlutterOpenGLTexture* opengl_texture) {
  const FlutterDesktopGpuSurfaceDescriptor* descriptor =
      texture_callback_(width, height, user_data_);

  if (!CreateOrUpdateTexture(descriptor)) {
    return false;
  }

  // Populate the texture object used by the engine.
  opengl_texture->target = GL_TEXTURE_2D;
  opengl_texture->name = gl_texture_;
  opengl_texture->format = is_rgba16float_ ? GL_RGBA16F_EXT : GL_RGBA8_OES;
  opengl_texture->destruction_callback = nullptr;
  opengl_texture->user_data = nullptr;
  opengl_texture->width = SAFE_ACCESS(descriptor, visible_width, 0);
  opengl_texture->height = SAFE_ACCESS(descriptor, visible_height, 0);

  return true;
}

void ExternalTextureD3d::ReleaseImage() {
  if (egl_surface_ != EGL_NO_SURFACE) {
    eglReleaseTexImage(egl_manager_->egl_display(), egl_surface_,
                       EGL_BACK_BUFFER);
    eglDestroySurface(egl_manager_->egl_display(), egl_surface_);
    egl_surface_ = EGL_NO_SURFACE;
  }
}

bool ExternalTextureD3d::CreateOrUpdateTexture(
    const FlutterDesktopGpuSurfaceDescriptor* descriptor) {
  if (descriptor == nullptr ||
      SAFE_ACCESS(descriptor, handle, nullptr) == nullptr) {
    ReleaseImage();
    return false;
  }

  if (gl_texture_ == 0) {
    gl_->GenTextures(1, &gl_texture_);

    gl_->BindTexture(GL_TEXTURE_2D, gl_texture_);
    gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  } else {
    gl_->BindTexture(GL_TEXTURE_2D, gl_texture_);
  }

  auto handle = SAFE_ACCESS(descriptor, handle, nullptr);
  if (handle != last_surface_handle_) {
    ReleaseImage();

    // Detect RGBA16Float textures for HDR support, and for FP16 shared
    // handles keep the ANGLE-device-opened texture alive so we can bind it
    // via the direct-pointer EGL path (pbuffer-from-share-handle rejects FP16).
    is_rgba16float_ = false;
    angle_device_texture_.Reset();
    if (type_ == kFlutterDesktopGpuSurfaceTypeD3d11Texture2D) {
      auto* d3d_texture = static_cast<ID3D11Texture2D*>(handle);
      D3D11_TEXTURE2D_DESC tex_desc;
      d3d_texture->GetDesc(&tex_desc);
      is_rgba16float_ =
          (tex_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
    } else if (type_ == kFlutterDesktopGpuSurfaceTypeDxgiSharedHandle) {
      // Open the shared resource on ANGLE's device: needed to query format,
      // and retained for FP16 so we can bind via EGL_D3D_TEXTURE_ANGLE.
      Microsoft::WRL::ComPtr<ID3D11Device> angle_device;
      if (const_cast<egl::Manager*>(egl_manager_)
              ->GetDevice(angle_device.GetAddressOf())) {
        HRESULT hr = angle_device->OpenSharedResource(
            static_cast<HANDLE>(handle), IID_PPV_ARGS(&angle_device_texture_));
        if (SUCCEEDED(hr) && angle_device_texture_) {
          D3D11_TEXTURE2D_DESC tex_desc;
          angle_device_texture_->GetDesc(&tex_desc);
          is_rgba16float_ =
              (tex_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
        }
      }
    }

    EGLint attributes[] = {
        EGL_WIDTH,
        static_cast<EGLint>(SAFE_ACCESS(descriptor, width, 0)),
        EGL_HEIGHT,
        static_cast<EGLint>(SAFE_ACCESS(descriptor, height, 0)),
        EGL_TEXTURE_TARGET,
        EGL_TEXTURE_2D,
        EGL_TEXTURE_FORMAT,
        EGL_TEXTURE_RGBA,
        EGL_NONE};

    // FP16 shared handles bind via the direct-pointer path against the
    // ANGLE-device-opened texture; 8-bit shared handles stay on the original
    // pbuffer-from-share-handle path (required for BGRA8).
    const bool use_direct_pointer =
        (type_ == kFlutterDesktopGpuSurfaceTypeD3d11Texture2D) ||
        (type_ == kFlutterDesktopGpuSurfaceTypeDxgiSharedHandle &&
         is_rgba16float_ && angle_device_texture_);

    EGLClientBuffer buffer =
        (type_ == kFlutterDesktopGpuSurfaceTypeDxgiSharedHandle &&
         is_rgba16float_ && angle_device_texture_)
            ? static_cast<EGLClientBuffer>(angle_device_texture_.Get())
            : handle;

    egl_surface_ = egl_manager_->CreateSurfaceFromHandle(
        use_direct_pointer ? EGL_D3D_TEXTURE_ANGLE
                           : EGL_D3D_TEXTURE_2D_SHARE_HANDLE_ANGLE,
        buffer, attributes);

    if (egl_surface_ == EGL_NO_SURFACE) {
      EGLint err = eglGetError();
      FML_LOG(ERROR) << "CreateSurfaceFromHandle failed: eglError=0x" << std::hex
                     << err << " direct=" << std::dec << use_direct_pointer
                     << " fp16=" << is_rgba16float_;
    } else if (eglBindTexImage(egl_manager_->egl_display(), egl_surface_,
                               EGL_BACK_BUFFER) == EGL_FALSE) {
      EGLint err = eglGetError();
      FML_LOG(ERROR) << "eglBindTexImage failed: eglError=0x" << std::hex << err
                     << " direct=" << std::dec << use_direct_pointer
                     << " fp16=" << is_rgba16float_;
    }
    last_surface_handle_ = handle;
  }

  auto release_callback = SAFE_ACCESS(descriptor, release_callback, nullptr);
  if (release_callback) {
    release_callback(SAFE_ACCESS(descriptor, release_context, nullptr));
  }
  return egl_surface_ != EGL_NO_SURFACE;
}

}  // namespace flutter
