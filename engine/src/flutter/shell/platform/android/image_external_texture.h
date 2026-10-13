// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_ANDROID_IMAGE_EXTERNAL_TEXTURE_H_
#define FLUTTER_SHELL_PLATFORM_ANDROID_IMAGE_EXTERNAL_TEXTURE_H_

#include <functional>
#include <memory>
#include <vector>

#include "flutter/common/graphics/texture.h"
#include "flutter/fml/logging.h"
#include "flutter/fml/task_runner.h"
#include "flutter/shell/platform/android/image_lru.h"
#include "flutter/shell/platform/android/jni/platform_view_android_jni.h"
#include "flutter/shell/platform/android/platform_view_android_jni_impl.h"

#include <android/hardware_buffer.h>
#include <android/hardware_buffer_jni.h>

namespace flutter {

//------------------------------------------------------------------------------
/// @brief      What a backend hands back for buffers a texture has stopped
///             drawing: a sync fence that signals once the GPU work that drew
///             them is done, or, when the backend can't export one, a check
///             that reports when that work is done.
///
struct ReleaseFence {
  /// A sync fence fd, or -1 when there is none.
  int fd = -1;
  /// When set (and `fd` is -1), returns true once the work is done. Called
  /// repeatedly on the raster thread until it does; never blocks.
  std::function<bool()> is_done;
};

//------------------------------------------------------------------------------
/// @brief      Makes release fences for one texture's backend. Shared with
///             the tasks that release the texture's buffers, so it can outlive
///             the texture.
///
class ReleaseFenceMaker {
 public:
  virtual ~ReleaseFenceMaker() = default;

  //----------------------------------------------------------------------------
  /// @brief      Called on the raster thread once the frame that last drew the
  ///             released buffers has been submitted.
  ///
  /// @param[in]  images     Backend objects for the released buffers, as
  ///                        passed to `SetSampledHardwareBuffer`; entries may
  ///                        be null.
  /// @param[in]  want_fence Whether anyone is waiting for the buffers. When
  ///                        false, return an empty `ReleaseFence` after any
  ///                        bookkeeping the backend needs.
  ///
  virtual ReleaseFence Create(const std::vector<std::shared_ptr<void>>& images,
                              bool want_fence) = 0;
};

//------------------------------------------------------------------------------
/// @brief      External texture peered to a sequence of
///             android.hardware.HardwareBuffers.
///
///             Android Hardware Buffers are available on newer versions of
///             Android (API 29 and above).
///
///             This is an abstract base class and graphics packages provide
///             concrete implementations of this class that bind hardware
///             buffers to their own package-specific implementations of
///             textures (SkImages, impeller::Texture, etc...).
///
///             Android Hardware Buffers allow binding to both OpenGL and Vulkan
///             client-rendering APIs in a zero copy manner. Because of this
///             graphics packages that support OpenGL and Vulkan can have
///             multiple subclasses for each supported client-rendering API.
///
class ImageExternalTexture : public flutter::Texture {
 public:
  /// Whether the last image should be reset when the context is destroyed.
  enum class ImageLifecycle { kReset, kKeepAlive };

  explicit ImageExternalTexture(
      int64_t id,
      const fml::jni::ScopedJavaGlobalRef<jobject>& image_texture_entry,
      const std::shared_ptr<PlatformViewAndroidJNI>& jni_facade,
      ImageLifecycle lifecycle,
      fml::RefPtr<fml::TaskRunner> raster_task_runner);

  /// Wait (up to 2 s) until a producer's acquire fence (a Linux sync_fd) is
  /// signaled, then close it. A no-op for `-1`.
  static void WaitOnAndCloseSyncFd(int fd);

  // |flutter::Texture|
  virtual ~ImageExternalTexture();

 protected:
  //----------------------------------------------------------------------------
  /// @brief      Subclasses override this method to bind the latest
  ///             representation of the hardware buffer wrapped by this texture
  ///             instance to their own graphics package specific texture
  ///             classes (SkImage, impeller::Texture, etc...).
  ///
  ///             After a call to this method, attachment state of this instance
  ///             must be kAttached. That is the responsibility of the subclass.
  ///
  /// @param      context  The context
  /// @param[in]  bounds   The bounds
  ///
  virtual void ProcessFrame(PaintContext& context, const SkRect& bounds) = 0;

  virtual void Attach(PaintContext& context) = 0;

  virtual void Detach() = 0;

  JavaLocalRef AcquireLatestImage();

  /// Direct-AHB intake. Returns an `AcquiredHardwareBuffer` whose `buffer` is
  /// nullptr when the producer is on the legacy `Image` flow (or when no
  /// frame has been pushed yet). When non-null, the consumer owns one
  /// `AHardwareBuffer_acquire` reference + the fence fd, per the contract
  /// on `AcquiredHardwareBuffer`.
  AcquiredHardwareBuffer AcquireLatestHardwareBuffer();

  /// Drop our one AHB reference (and close the acquire fence fd, if any).
  /// Safe to call with `handle.buffer == nullptr` as a no-op. Does not hand
  /// the buffer back to its producer; see `SetSampledHardwareBuffer`.
  void ReleaseAcquiredHardwareBuffer(const AcquiredHardwareBuffer& handle);

  /// Record that `buffer` (an acquired buffer, or nullptr for a frame that
  /// came from the `Image` flow) is what this texture draws from now on, and
  /// hand the buffer it replaces back to its producer. The release happens
  /// after the current frame has been submitted, behind a fence that covers
  /// every draw of that buffer, including any already recorded this frame.
  ///
  /// `image` is the backend's object for `buffer` (see `ReleaseFenceMaker`),
  /// and `wants_release` whether its producer listens for releases.
  void SetSampledHardwareBuffer(AHardwareBuffer* buffer,
                                std::shared_ptr<void> image = nullptr,
                                bool wants_release = true);

  /// Hand `buffer` back to its producer at once, with no fence: for a pushed
  /// buffer this texture never drew.
  void ReleaseUnsampled(AHardwareBuffer* buffer);

  /// The pushed buffer this texture currently draws, or nullptr.
  AHardwareBuffer* sampled_hardware_buffer() const;

  /// Makes this texture's release fences. Called once, on first use.
  virtual std::unique_ptr<ReleaseFenceMaker> CreateReleaseFenceMaker() = 0;

  void CloseImage(const fml::jni::JavaRef<jobject>& image);

  JavaLocalRef HardwareBufferFor(const fml::jni::JavaRef<jobject>& image);

  void CloseHardwareBuffer(const fml::jni::JavaRef<jobject>& hardware_buffer);

  AHardwareBuffer* AHardwareBufferFor(
      const fml::jni::JavaRef<jobject>& hardware_buffer);

  fml::jni::ScopedJavaGlobalRef<jobject> image_texture_entry_;
  std::shared_ptr<PlatformViewAndroidJNI> jni_facade_;

  enum class AttachmentState { kUninitialized, kAttached, kDetached };
  AttachmentState state_ = AttachmentState::kUninitialized;
  sk_sp<flutter::DlImage> dl_image_;
  ImageLRU image_lru_ = ImageLRU();

 private:
  class BufferReleaser;

  BufferReleaser& GetReleaser();

  // |flutter::Texture|.
  void Paint(PaintContext& context,
             const DlRect& bounds,
             bool freeze,
             const DlImageSampling sampling) override;

  // |flutter::Texture|.
  void MarkNewFrameAvailable() override;

  // |flutter::Texture|
  void OnTextureUnregistered() override;

  // |flutter::ContextListener|
  void OnGrContextCreated() override;

  // |flutter::ContextListener|
  void OnGrContextDestroyed() override;

  const ImageLifecycle texture_lifecycle_;
  const fml::RefPtr<fml::TaskRunner> raster_task_runner_;

  /// The pushed buffer this texture currently draws, or nullptr. Identity
  /// only: the engine's reference on it was dropped once it was bound.
  AHardwareBuffer* sampled_hardware_buffer_ = nullptr;
  std::shared_ptr<void> sampled_image_;
  bool sampled_wants_release_ = false;

  std::shared_ptr<BufferReleaser> releaser_;

  FML_DISALLOW_COPY_AND_ASSIGN(ImageExternalTexture);
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_ANDROID_IMAGE_EXTERNAL_TEXTURE_H_
