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
      ImageExternalTexture::ImageLifecycle lifecycle);

  ~ImageExternalTextureVKImpeller() override;

 private:
  void Attach(PaintContext& context) override;
  void ProcessFrame(PaintContext& context, const SkRect& bounds) override;
  void Detach() override;

  /// Shared Vulkan pipeline work for both `pushImage` and
  /// `pushHardwareBuffer` intake paths: LRU lookup, `AHBTextureSourceVK`
  /// construction, and the shader-read layout transition. Caller-level
  /// cleanup of the Java `HardwareBuffer` wrapper or the transferred
  /// `AHardwareBuffer` reference runs via `fml::ScopedCleanupClosure` in
  /// `ProcessFrame`.
  ///
  /// `release_ack_fd` is the producer's `eventfd` (or `-1`). This
  /// function takes ownership: on every exit path the fd is either
  /// signaled+closed immediately (early return / error) or stashed in
  /// `pending_release_ack_fd_`, to be signaled the next time
  /// `IngestHardwareBuffer` is called (or the texture is destroyed).
  /// This one-frame delay gives the compositor's render pass from the
  /// previous `Ingest` time to finish sampling the old AHB before the
  /// producer reuses the slot — a tighter binding (signaling on the
  /// actual composite completion) would need Impeller-side hooks we
  /// don't have today. After return the caller must NOT touch the fd.
  void IngestHardwareBuffer(AHardwareBuffer* ahb, int release_ack_fd);

  /// One-frame-deferred release-ack fd. See `IngestHardwareBuffer` doc
  /// for the reasoning. `-1` when no frame is pending.
  int pending_release_ack_fd_ = -1;

  const std::shared_ptr<impeller::ContextVK> impeller_context_;
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_ANDROID_IMAGE_EXTERNAL_TEXTURE_VK_IMPELLER_H_
