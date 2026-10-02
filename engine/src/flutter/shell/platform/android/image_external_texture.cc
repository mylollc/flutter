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

// How often a release that has no fence to hand over re-checks whether the
// GPU work it waits for is done.
constexpr fml::TimeDelta kReleasePollInterval =
    fml::TimeDelta::FromMilliseconds(1);

}  // namespace

// Hands the buffers a texture has stopped drawing back to their producer once
// the frame that last drew them has been submitted: a task posted to the
// raster task runner runs after the current raster task, which submits the
// whole frame. Without a task runner (only in teardown), releases happen at
// once. Shared by the texture and the tasks it posts, so a release still
// completes if the texture goes away first. Raster thread only.
class ImageExternalTexture::BufferReleaser
    : public std::enable_shared_from_this<BufferReleaser> {
 public:
  BufferReleaser(std::shared_ptr<PlatformViewAndroidJNI> jni_facade,
                 const fml::jni::ScopedJavaGlobalRef<jobject>& entry,
                 std::unique_ptr<ReleaseFenceMaker> maker,
                 fml::RefPtr<fml::TaskRunner> raster_task_runner)
      : jni_facade_(std::move(jni_facade)),
        entry_(entry),
        maker_(std::move(maker)),
        raster_task_runner_(std::move(raster_task_runner)) {}

  ~BufferReleaser() {
    // No task is pending by now, so these are releases whose GPU work had not
    // finished when the last owner let go; nothing will draw the buffers
    // again, so they go back now.
    for (const Deferred& deferred : deferred_) {
      for (AHardwareBuffer* buffer : deferred.buffers) {
        Release(buffer, -1);
      }
    }
  }

  // Queue `buffer` for release once the current frame has been submitted.
  void Defer(AHardwareBuffer* buffer,
             std::shared_ptr<void> image,
             bool wants_release) {
    pending_.push_back({buffer, std::move(image), wants_release});
    if (flush_posted_) {
      return;
    }
    if (!raster_task_runner_) {
      Flush();
      return;
    }
    flush_posted_ = true;
    raster_task_runner_->PostTask(
        [self = shared_from_this()]() { self->Flush(); });
  }

  // Tell the producer it may reuse `buffer` once `fence_fd` (owned by the
  // callee; -1 = now) signals.
  void Release(AHardwareBuffer* buffer, int fence_fd) {
    // A null entry (a texture with no Java side) yields an empty reference
    // rather than attaching to the JVM for nothing.
    jni_facade_->ImageProducerTextureEntryOnHardwareBufferReleased(
        entry_.is_null() ? JavaLocalRef() : JavaLocalRef(entry_), buffer,
        fence_fd);
  }

 private:
  struct Pending {
    AHardwareBuffer* buffer;
    std::shared_ptr<void> image;
    bool wants_release;
  };

  struct Deferred {
    std::vector<AHardwareBuffer*> buffers;
    std::function<bool()> is_done;
  };

  void Flush() {
    flush_posted_ = false;
    if (pending_.empty()) {
      return;
    }
    std::vector<std::shared_ptr<void>> images;
    std::vector<AHardwareBuffer*> to_release;
    for (Pending& pending : pending_) {
      images.push_back(std::move(pending.image));
      if (pending.wants_release) {
        to_release.push_back(pending.buffer);
      }
    }
    pending_.clear();

    ReleaseFence fence = maker_->Create(images, !to_release.empty());
    if (to_release.empty()) {
      if (fence.fd >= 0) {
        ::close(fence.fd);
      }
      return;
    }
    if (fence.fd < 0 && fence.is_done && !fence.is_done()) {
      if (raster_task_runner_) {
        deferred_.push_back({std::move(to_release), std::move(fence.is_done)});
        SchedulePoll();
        return;
      }
    }
    for (size_t i = 0; i < to_release.size(); i++) {
      int fd = fence.fd;
      if (fd >= 0 && i + 1 < to_release.size()) {
        // Every release owns its fd; the last one takes the original.
        fd = ::dup(fence.fd);
        if (fd < 0) {
          FML_LOG(ERROR) << "Could not duplicate a release fence (errno="
                         << errno << "); waiting for it on the CPU.";
          struct pollfd pfd = {.fd = fence.fd, .events = POLLIN, .revents = 0};
          ::poll(&pfd, 1, kSyncFenceTimeoutMs);
        }
      }
      Release(to_release[i], fd);
    }
  }

  void SchedulePoll() {
    if (poll_posted_) {
      return;
    }
    poll_posted_ = true;
    raster_task_runner_->PostDelayedTask(
        [self = shared_from_this()]() { self->Poll(); }, kReleasePollInterval);
  }

  void Poll() {
    poll_posted_ = false;
    for (auto it = deferred_.begin(); it != deferred_.end();) {
      if (it->is_done()) {
        for (AHardwareBuffer* buffer : it->buffers) {
          Release(buffer, -1);
        }
        it = deferred_.erase(it);
      } else {
        ++it;
      }
    }
    if (!deferred_.empty()) {
      SchedulePoll();
    }
  }

  const std::shared_ptr<PlatformViewAndroidJNI> jni_facade_;
  const fml::jni::ScopedJavaGlobalRef<jobject> entry_;
  const std::unique_ptr<ReleaseFenceMaker> maker_;
  const fml::RefPtr<fml::TaskRunner> raster_task_runner_;
  std::vector<Pending> pending_;
  std::vector<Deferred> deferred_;
  bool flush_posted_ = false;
  bool poll_posted_ = false;

  FML_DISALLOW_COPY_AND_ASSIGN(BufferReleaser);
};

// `poll` with `POLLIN` is how the Android sync framework surfaces signal
// readiness on a sync_fd; `sync_wait` would do the same via a slightly
// higher-level wrapper but isn't part of the NDK base surface we target.
void ImageExternalTexture::WaitOnAndCloseSyncFd(int fd) {
  if (fd < 0) {
    return;
  }
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
                   << "; sampling may race producer GPU writes";
  } else if (rc == 0) {
    FML_LOG(ERROR) << "poll on sync_fd " << fd << " timed out after "
                   << kSyncFenceTimeoutMs << "ms; producer fence never "
                   << "signaled; sampling may race GPU writes for this frame";
  }
  ::close(fd);
}

ImageExternalTexture::ImageExternalTexture(
    int64_t id,
    const fml::jni::ScopedJavaGlobalRef<jobject>& image_texture_entry,
    const std::shared_ptr<PlatformViewAndroidJNI>& jni_facade,
    ImageLifecycle lifecycle,
    fml::RefPtr<fml::TaskRunner> raster_task_runner)
    : Texture(id),
      image_texture_entry_(image_texture_entry),
      jni_facade_(jni_facade),
      texture_lifecycle_(lifecycle),
      raster_task_runner_(std::move(raster_task_runner)) {}

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
        // Nothing samples the pushed buffer any more.
        SetSampledHardwareBuffer(nullptr);
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

ImageExternalTexture::BufferReleaser& ImageExternalTexture::GetReleaser() {
  if (!releaser_) {
    releaser_ = std::make_shared<BufferReleaser>(
        jni_facade_, image_texture_entry_, CreateReleaseFenceMaker(),
        raster_task_runner_);
  }
  return *releaser_;
}

void ImageExternalTexture::SetSampledHardwareBuffer(AHardwareBuffer* buffer,
                                                    std::shared_ptr<void> image,
                                                    bool wants_release) {
  if (buffer != nullptr && buffer == sampled_hardware_buffer_) {
    // The producer pushed the buffer on screen again. Every push is released
    // once, so the earlier one goes back, but the buffer stays in use: no
    // backend object goes with it, so the backend keeps it as it is.
    GetReleaser().Defer(buffer, nullptr, sampled_wants_release_);
    sampled_wants_release_ = wants_release;
    return;
  }
  if (sampled_hardware_buffer_ != nullptr) {
    GetReleaser().Defer(sampled_hardware_buffer_, std::move(sampled_image_),
                        sampled_wants_release_);
  }
  sampled_hardware_buffer_ = buffer;
  sampled_image_ = buffer != nullptr ? std::move(image) : nullptr;
  sampled_wants_release_ = buffer != nullptr && wants_release;
}

void ImageExternalTexture::ReleaseUnsampled(AHardwareBuffer* buffer) {
  GetReleaser().Release(buffer, /*fence_fd=*/-1);
}

AHardwareBuffer* ImageExternalTexture::sampled_hardware_buffer() const {
  return sampled_hardware_buffer_;
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
