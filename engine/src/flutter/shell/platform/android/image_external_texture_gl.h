// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_ANDROID_IMAGE_EXTERNAL_TEXTURE_GL_H_
#define FLUTTER_SHELL_PLATFORM_ANDROID_IMAGE_EXTERNAL_TEXTURE_GL_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>

#include "flutter/impeller/geometry/size.h"
#include "flutter/impeller/toolkit/egl/image.h"
#include "flutter/impeller/toolkit/gles/texture.h"
#include "flutter/shell/platform/android/image_external_texture.h"

namespace flutter {

class ImageExternalTextureGL : public ImageExternalTexture {
 public:
  ImageExternalTextureGL(
      int64_t id,
      const fml::jni::ScopedJavaGlobalRef<jobject>& image_textury_entry,
      const std::shared_ptr<PlatformViewAndroidJNI>& jni_facade,
      ImageExternalTexture::ImageLifecycle lifecycle);

  ~ImageExternalTextureGL() override;

 protected:
  /// What the direct-AHB intake knows about a buffer besides its EGLImage.
  /// The `Image` intake passes a default-constructed one, which keeps that
  /// path's upstream behavior (paint-bounds size, RGBA8, sRGB).
  struct BufferInfo {
    /// The buffer's size in pixels; empty when unknown.
    impeller::ISize size;
    /// The buffer's `AHardwareBuffer_Format`; 0 when unknown.
    uint32_t format = 0;
    /// Producer-declared color space code (impeller::ColorSpace);
    /// -1 == unspecified, infer from `format`.
    int color_space = -1;
  };

  // |ImageExternalTexture|
  void Attach(PaintContext& context) override;

  // |ImageExternalTexture|
  void Detach() override;

  // |ImageExternalTexture|
  void ProcessFrame(PaintContext& context, const SkRect& bounds) override;

  virtual sk_sp<flutter::DlImage> CreateDlImage(
      PaintContext& context,
      const SkRect& bounds,
      std::optional<HardwareBufferKey> id,
      impeller::UniqueEGLImageKHR&& egl_image,
      const BufferInfo& info) = 0;

  void UpdateImage(JavaLocalRef& hardware_buffer,
                   const SkRect& bounds,
                   PaintContext& context);

  void UpdateImage(AHardwareBuffer* hardware_buffer,
                   const SkRect& bounds,
                   PaintContext& context,
                   const BufferInfo& info);

  impeller::UniqueEGLImageKHR CreateEGLImage(AHardwareBuffer* buffer);

  struct GlEntry {
    impeller::UniqueEGLImageKHR egl_image;
    impeller::UniqueGLTexture texture;
  };

  // Each GL entry is keyed off of the currently active
  // hardware buffers and evicted when the hardware buffer
  // is removed from the LRU cache.
  std::unordered_map<HardwareBufferKey, GlEntry> gl_entries_;

 private:
  /// Direct-AHB intake for `pushHardwareBuffer` producers: waits on the
  /// acquire fence, binds the buffer, and defers its release-ack until the
  /// next frame. Takes ownership of every resource in `direct`.
  void IngestHardwareBuffer(AcquiredHardwareBuffer direct,
                            const SkRect& bounds,
                            PaintContext& context);

  /// Signals `pending_release_ack_fd_` once the GPU has finished every
  /// command submitted so far on the current EGL context, which includes
  /// the draws that sampled the previous buffer.
  void SignalPendingReleaseAck();

  /// One-frame-deferred release-ack fd, as in the Vulkan intake. `-1` when
  /// no frame is pending.
  int pending_release_ack_fd_ = -1;

  FML_DISALLOW_COPY_AND_ASSIGN(ImageExternalTextureGL);
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_ANDROID_IMAGE_EXTERNAL_TEXTURE_GL_H_
