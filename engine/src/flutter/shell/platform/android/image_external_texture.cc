// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/android/image_external_texture.h"

#include <android/hardware_buffer.h>
#include <android/hardware_buffer_jni.h>
#include <android/sensor.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>

#include "flutter/fml/platform/android/jni_util.h"
#include "flutter/impeller/toolkit/android/proc_table.h"
#include "flutter/shell/platform/android/jni/platform_view_android_jni.h"

namespace flutter {

namespace {

// Producer-fence wait timeout. Normal signal latency is microseconds;
// this bound just keeps a stuck driver or hung producer from freezing
// the raster thread indefinitely. Exceeding it is unusual enough to
// warrant an error log; the sampling that follows may race producer
// GPU writes for one frame, typically self-correcting on the next.
constexpr int kSyncFenceTimeoutMs = 2000;

}  // namespace

// `poll` with `POLLIN` is how the Android sync framework surfaces signal
// readiness on a sync_fd; `sync_wait` would do the same via a slightly
// higher-level wrapper but isn't part of the NDK base surface we target.
void ImageExternalTexture::WaitOnAndCloseSyncFd(int fd) {
  struct pollfd pfd;
  pfd.fd = fd;
  pfd.events = POLLIN;
  pfd.revents = 0;
  int rc;
  do {
    rc = ::poll(&pfd, 1, kSyncFenceTimeoutMs);
  } while (rc == -1 && errno == EINTR);
  if (rc < 0) {
    FML_LOG(ERROR) << "poll on sync_fd " << fd << " failed: errno=" << errno
                   << " — sampling may race producer GPU writes";
  } else if (rc == 0) {
    FML_LOG(ERROR) << "poll on sync_fd " << fd << " timed out after "
                   << kSyncFenceTimeoutMs << "ms — producer fence never "
                   << "signaled; sampling may race GPU writes for this frame";
  }
  ::close(fd);
}

// The release-ack fd is an `eventfd`: an 8-byte counter increment wakes
// the producer's `poll(POLLIN)`.
void ImageExternalTexture::SignalAndCloseAckFd(int fd) {
  if (fd < 0) {
    return;
  }
  const uint64_t one = 1;
  (void)::write(fd, &one, sizeof(one));
  ::close(fd);
}

ImageExternalTexture::ImageExternalTexture(
    int64_t id,
    const fml::jni::ScopedJavaGlobalRef<jobject>& image_texture_entry,
    const std::shared_ptr<PlatformViewAndroidJNI>& jni_facade,
    ImageLifecycle lifecycle)
    : Texture(id),
      image_texture_entry_(image_texture_entry),
      jni_facade_(jni_facade),
      texture_lifecycle_(lifecycle) {}

ImageExternalTexture::~ImageExternalTexture() = default;

// Implementing flutter::Texture.
void ImageExternalTexture::Paint(PaintContext& context,
                                 const DlRect& bounds,
                                 bool freeze,
                                 const DlImageSampling sampling) {
  if (state_ == AttachmentState::kDetached) {
    return;
  }
  Attach(context);
  const bool should_process_frame = !freeze;
  if (should_process_frame) {
    ProcessFrame(context, ToSkRect(bounds));
  }
  if (dl_image_) {
    context.canvas->DrawImageRect(
        dl_image_,                             // image
        DlRect::Make(dl_image_->GetBounds()),  // source rect
        bounds,                                // destination rect
        sampling,                              // sampling
        context.paint,                         // paint
        flutter::DlSrcRectConstraint::kStrict  // enforce edges
    );
  } else {
    FML_LOG(INFO) << "No DlImage available for ImageExternalTexture to paint.";
  }
}

// Implementing flutter::Texture.
void ImageExternalTexture::MarkNewFrameAvailable() {
  // NOOP.
}

// Implementing flutter::Texture.
void ImageExternalTexture::OnTextureUnregistered() {}

// Implementing flutter::ContextListener.
void ImageExternalTexture::OnGrContextCreated() {
  state_ = AttachmentState::kUninitialized;
}

// Implementing flutter::ContextListener.
void ImageExternalTexture::OnGrContextDestroyed() {
  if (state_ == AttachmentState::kAttached) {
    switch (texture_lifecycle_) {
      case ImageLifecycle::kReset: {
        dl_image_.reset();
        image_lru_.Clear();
      } break;
      case ImageLifecycle::kKeepAlive:
        // Intentionally do nothing.
        ///
        // If we reset the image, we are not able to re-acquire it, but the
        // producer of the image will not know to reproduce it, resulting in a
        // blank image. See https://github.com/flutter/flutter/issues/163561.
        break;
    }
    Detach();
  }
  state_ = AttachmentState::kDetached;
}

JavaLocalRef ImageExternalTexture::AcquireLatestImage() {
  JNIEnv* env = fml::jni::AttachCurrentThread();
  FML_CHECK(env != nullptr);

  // ImageTextureEntry.acquireLatestImage.
  JavaLocalRef image_java =
      jni_facade_->ImageProducerTextureEntryAcquireLatestImage(
          JavaLocalRef(image_texture_entry_));
  return image_java;
}

AcquiredHardwareBuffer ImageExternalTexture::AcquireLatestHardwareBuffer() {
  return jni_facade_->ImageProducerTextureEntryAcquireLatestHardwareBuffer(
      JavaLocalRef(image_texture_entry_));
}

void ImageExternalTexture::ReleaseAcquiredHardwareBuffer(
    const AcquiredHardwareBuffer& handle) {
  if (handle.acquire_fence_fd >= 0) {
    // Safety-net close only — the VK Impeller path consumes the fd via
    // `WaitOnAndCloseSyncFd` in `ProcessFrame` before the sampling
    // barrier and zeroes this field, so this branch should not fire in
    // practice. If a future backend forwards a non-negative fd here
    // without consuming it upstream, we at least avoid the leak.
    ::close(handle.acquire_fence_fd);
  }
  if (handle.release_ack_fd >= 0) {
    // Safety-net signal+close. The VK Impeller path normally arms a
    // completion callback that signals this fd when GPU sampling
    // finishes, and zeroes it here. Reaching this branch with a valid
    // fd means IngestHardwareBuffer errored before arming the
    // callback; we signal so the producer doesn't block forever on its
    // `poll(POLLIN)`.
    const uint64_t one = 1;
    (void)::write(handle.release_ack_fd, &one, sizeof(one));
    ::close(handle.release_ack_fd);
  }
  if (handle.buffer != nullptr) {
    // Drop the reference the engine transferred to us. The texture source
    // (e.g. `AHBTextureSourceVK`) holds its own independent reference via
    // `AHardwareBuffer_acquire` during construction, so the buffer stays
    // alive for as long as Impeller needs it.
    const auto& release =
        impeller::android::GetProcTable().AHardwareBuffer_release;
    if (release) {
      release(handle.buffer);
    } else {
      // Not expected in practice — `AHardwareBuffer_release` is API 26+
      // and the direct-AHB path itself is API 26+-gated. A missing proc
      // here signals a malformed device environment; log rather than
      // silently leaking the reference.
      FML_LOG(WARNING)
          << "AHardwareBuffer_release unavailable; leaking AHB reference.";
    }
  }
}

void ImageExternalTexture::CloseImage(const fml::jni::JavaRef<jobject>& image) {
  if (image.obj() == nullptr) {
    return;
  }
  jni_facade_->ImageClose(JavaLocalRef(image));
}

void ImageExternalTexture::CloseHardwareBuffer(
    const fml::jni::JavaRef<jobject>& hardware_buffer) {
  if (hardware_buffer.obj() == nullptr) {
    return;
  }
  jni_facade_->HardwareBufferClose(JavaLocalRef(hardware_buffer));
}

JavaLocalRef ImageExternalTexture::HardwareBufferFor(
    const fml::jni::JavaRef<jobject>& image) {
  if (image.obj() == nullptr) {
    return JavaLocalRef();
  }
  // Image.getHardwareBuffer.
  return jni_facade_->ImageGetHardwareBuffer(JavaLocalRef(image));
}

AHardwareBuffer* ImageExternalTexture::AHardwareBufferFor(
    const fml::jni::JavaRef<jobject>& hardware_buffer) {
  JNIEnv* env = fml::jni::AttachCurrentThread();
  FML_CHECK(env != nullptr);
  const auto& proc =
      impeller::android::GetProcTable().AHardwareBuffer_fromHardwareBuffer;
  return proc ? proc(env, hardware_buffer.obj()) : nullptr;
}

}  // namespace flutter
