// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/android/image_external_texture_gl_impeller.h"

#include "flutter/impeller/display_list/dl_image_impeller.h"
#include "flutter/impeller/renderer/backend/gles/texture_gles.h"

namespace flutter {

ImageExternalTextureGLImpeller::ImageExternalTextureGLImpeller(
    const std::shared_ptr<impeller::ContextGLES>& context,
    int64_t id,
    const fml::jni::ScopedJavaGlobalRef<jobject>& image_textury_entry,
    const std::shared_ptr<PlatformViewAndroidJNI>& jni_facade,
    ImageExternalTexture::ImageLifecycle lifecycle)
    : ImageExternalTextureGL(id, image_textury_entry, jni_facade, lifecycle),
      impeller_context_(context) {}

void ImageExternalTextureGLImpeller::Detach() {}

void ImageExternalTextureGLImpeller::Attach(PaintContext& context) {
  if (state_ == AttachmentState::kUninitialized) {
    ImageExternalTextureGL::Attach(context);
  }
}

sk_sp<flutter::DlImage> ImageExternalTextureGLImpeller::CreateDlImage(
    PaintContext& context,
    const SkRect& bounds,
    std::optional<HardwareBufferKey> id,
    impeller::UniqueEGLImageKHR&& egl_image,
    const BufferInfo& info) {
  impeller::TextureDescriptor desc;
  desc.type = impeller::TextureType::kTextureExternalOES;
  desc.storage_mode = impeller::StorageMode::kDevicePrivate;
  desc.format = impeller::PixelFormat::kR8G8B8A8UNormInt;
  desc.size = {static_cast<int>(bounds.width()),
               static_cast<int>(bounds.height())};
  desc.mip_count = 1;
  // Direct-AHB intake: describe the buffer itself, as the Vulkan texture
  // source (`AHBTextureSourceVK`) does, so TextureContents converts its
  // color space when it draws the texture.
  if (!info.size.IsEmpty()) {
    desc.size = info.size;
  }
  if (info.format == AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT) {
    desc.format = impeller::PixelFormat::kR16G16B16A16Float;
  }
  if (info.color_space >= 0) {
    desc.color_space = static_cast<impeller::ColorSpace>(info.color_space);
  } else if (desc.format == impeller::PixelFormat::kR16G16B16A16Float) {
    // Same inference as the Vulkan texture source: an F16 buffer with no
    // declared color space is linear Display P3.
    desc.color_space = impeller::ColorSpace::kLinearDisplayP3;
  }
  auto texture = std::make_shared<impeller::TextureGLES>(
      impeller_context_->GetReactor(), desc);
  // The contents will be initialized later in the call to
  // `glEGLImageTargetTexture2DOES` instead of by Impeller.
  texture->MarkContentsInitialized();
  texture->SetCoordinateSystem(
      impeller::TextureCoordinateSystem::kUploadFromHost);
  if (!texture->Bind()) {
    return nullptr;
  }
  // Associate the hardware buffer image with the texture.
  glEGLImageTargetTexture2DOES(
      GL_TEXTURE_EXTERNAL_OES,
      static_cast<GLeglImageOES>(egl_image.get().image));
  gl_entries_[id.value_or(0)] = GlEntry{
      .egl_image = std::move(egl_image),
  };
  return impeller::DlImageImpeller::Make(texture);
}

}  // namespace flutter
