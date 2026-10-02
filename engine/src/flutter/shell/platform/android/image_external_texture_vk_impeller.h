// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_ANDROID_IMAGE_EXTERNAL_TEXTURE_VK_IMPELLER_H_
#define FLUTTER_SHELL_PLATFORM_ANDROID_IMAGE_EXTERNAL_TEXTURE_VK_IMPELLER_H_

#include <cstdint>
#include <utility>
#include "flutter/shell/platform/android/image_external_texture.h"

#include "flutter/impeller/renderer/backend/vulkan/android/ahb_texture_source_vk.h"
#include "flutter/impeller/renderer/backend/vulkan/context_vk.h"
#include "flutter/impeller/renderer/backend/vulkan/texture_vk.h"
#include "flutter/impeller/renderer/backend/vulkan/vk.h"
#include "flutter/shell/platform/android/android_context_vk_impeller.h"

namespace flutter {

class ImageExternalTextureVKImpeller : public ImageExternalTexture {
 public:
  ImageExternalTextureVKImpeller(
      const std::shared_ptr<impeller::ContextVK>& impeller_context,
      int64_t id,
      const fml::jni::ScopedJavaGlobalRef<jobject>&
          hardware_buffer_texture_entry,
      const std::shared_ptr<PlatformViewAndroidJNI>& jni_facade,
      ImageExternalTexture::ImageLifecycle lifecycle,
      fml::RefPtr<fml::TaskRunner> raster_task_runner);

  ~ImageExternalTextureVKImpeller() override;

 private:
  void Attach(PaintContext& context) override;
  void ProcessFrame(PaintContext& context, const SkRect& bounds) override;
  void Detach() override;

  // |ImageExternalTexture|
  std::unique_ptr<ReleaseFenceMaker> CreateReleaseFenceMaker() override;

  /// Shared Vulkan pipeline work for both `pushImage` and
  /// `pushHardwareBuffer` intake paths: LRU lookup, `AHBTextureSourceVK`
  /// construction, and the transition to shader read. Caller-level cleanup
  /// of the Java `HardwareBuffer` wrapper or the transferred
  /// `AHardwareBuffer` reference runs via `fml::ScopedCleanupClosure` in
  /// `ProcessFrame`.
  ///
  /// `color_space` is the producer-declared color-space code (impeller
  /// ColorSpace; -1 == unspecified, infer from the buffer format).
  /// `acquire_ownership` takes the image from the producer's foreign queue
  /// family, for buffers that are handed back to it on release.
  ///
  /// Returns the texture now drawn, or nullptr (with `dl_image_` unchanged)
  /// on failure.
  std::shared_ptr<impeller::TextureVK> IngestHardwareBuffer(
      AHardwareBuffer* ahb,
      int color_space,
      bool acquire_ownership);

  const std::shared_ptr<impeller::ContextVK> impeller_context_;
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_ANDROID_IMAGE_EXTERNAL_TEXTURE_VK_IMPELLER_H_
