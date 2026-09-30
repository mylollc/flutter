// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/android/image_external_texture_gl.h"

#include <android/hardware_buffer_jni.h>
#include <android/sensor.h>

#include "flutter/common/graphics/texture.h"
#include "flutter/fml/closure.h"
#include "flutter/fml/logging.h"
#include "flutter/impeller/core/formats.h"
#include "flutter/impeller/display_list/dl_image_impeller.h"
#include "flutter/impeller/toolkit/android/hardware_buffer.h"
#include "flutter/impeller/toolkit/egl/image.h"
#include "flutter/impeller/toolkit/gles/texture.h"
#include "third_party/skia/include/core/SkAlphaType.h"
#include "third_party/skia/include/core/SkColorType.h"
#include "third_party/skia/include/gpu/ganesh/SkImageGanesh.h"
#include "third_party/skia/include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "third_party/skia/include/gpu/ganesh/gl/GrGLTypes.h"

namespace flutter {

namespace {

// Bound on the release-ack GPU wait, matching the producer's own 2 s wait
// for the ack. The draws it waits on were submitted a frame ago, so this
// normally returns at once.
constexpr EGLTimeKHR kReleaseAckGpuWaitTimeoutNs = 2'000'000'000;

// Blocks until the GPU has finished every command submitted so far on the
// current EGL context. Returns at once when no context is current: then
// nothing of ours can still be in flight on it.
void WaitForSubmittedGpuWork() {
  EGLDisplay display = eglGetCurrentDisplay();
  if (display == EGL_NO_DISPLAY || eglGetCurrentContext() == EGL_NO_CONTEXT) {
    return;
  }
  EGLSyncKHR sync = eglCreateSyncKHR(display, EGL_SYNC_FENCE_KHR, nullptr);
  if (sync == EGL_NO_SYNC_KHR) {
    // No EGL_KHR_fence_sync: glFinish is slower but just as correct.
    glFinish();
    return;
  }
  const EGLint status =
      eglClientWaitSyncKHR(display, sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR,
                           kReleaseAckGpuWaitTimeoutNs);
  if (status != EGL_CONDITION_SATISFIED_KHR) {
    FML_LOG(ERROR) << "Release-ack fence wait failed (status=" << status
                   << "); releasing the buffer to its producer anyway.";
  }
  eglDestroySyncKHR(display, sync);
}

}  // namespace

ImageExternalTextureGL::ImageExternalTextureGL(
    int64_t id,
    const fml::jni::ScopedJavaGlobalRef<jobject>& image_texture_entry,
    const std::shared_ptr<PlatformViewAndroidJNI>& jni_facade,
    ImageExternalTexture::ImageLifecycle lifecycle)
    : ImageExternalTexture(id, image_texture_entry, jni_facade, lifecycle) {}

ImageExternalTextureGL::~ImageExternalTextureGL() {
  // As in the Vulkan intake: by the time the texture is destroyed, the
  // draws that sampled its last buffer have long completed, so the producer
  // can have the buffer back without a fence.
  SignalAndCloseAckFd(pending_release_ack_fd_);
  pending_release_ack_fd_ = -1;
}

void ImageExternalTextureGL::Attach(PaintContext& context) {
  if (state_ == AttachmentState::kUninitialized) {
    // TODO(johnmccurtchan): We currently display the first frame after an
    // attach-detach cycle as blank. There seems to be an issue on some
    // devices where ImageReaders/Images from before the detach aren't
    // valid after the attach. According to Android folks this doesn't
    // match the spec. Revisit this in the future.
    // See https://github.com/flutter/flutter/issues/142978 and
    // https://github.com/flutter/flutter/issues/139039.
    state_ = AttachmentState::kAttached;
  }
}

void ImageExternalTextureGL::UpdateImage(JavaLocalRef& hardware_buffer,
                                         const SkRect& bounds,
                                         PaintContext& context) {
  UpdateImage(AHardwareBufferFor(hardware_buffer), bounds, context,
              BufferInfo{});
}

void ImageExternalTextureGL::UpdateImage(AHardwareBuffer* hardware_buffer,
                                         const SkRect& bounds,
                                         PaintContext& context,
                                         const BufferInfo& info) {
  std::optional<HardwareBufferKey> key =
      impeller::android::HardwareBuffer::GetSystemUniqueID(hardware_buffer);
  auto existing_image = image_lru_.FindImage(key);
  if (existing_image != nullptr) {
    dl_image_ = existing_image;
    return;
  }

  auto egl_image = CreateEGLImage(hardware_buffer);
  if (!egl_image.is_valid()) {
    return;
  }

  dl_image_ = CreateDlImage(context, bounds, key, std::move(egl_image), info);
  if (key.has_value()) {
    gl_entries_.erase(image_lru_.AddImage(dl_image_, key.value()));
  }
}

void ImageExternalTextureGL::ProcessFrame(PaintContext& context,
                                          const SkRect& bounds) {
  // Prefer the direct-AHB path: `pushHardwareBuffer` leaves the `Image`
  // slot empty, so a producer on that path would otherwise draw nothing.
  // Falling through to the `Image` path when no raw AHB is pending keeps
  // producers that push via `pushImage` working.
  AcquiredHardwareBuffer direct = AcquireLatestHardwareBuffer();
  if (direct.buffer != nullptr) {
    IngestHardwareBuffer(direct, bounds, context);
    return;
  }

  JavaLocalRef image = AcquireLatestImage();
  if (image.is_null()) {
    return;
  }
  JavaLocalRef hardware_buffer = HardwareBufferFor(image);
  UpdateImage(hardware_buffer, bounds, context);
  CloseHardwareBuffer(hardware_buffer);
}

void ImageExternalTextureGL::IngestHardwareBuffer(AcquiredHardwareBuffer direct,
                                                  const SkRect& bounds,
                                                  PaintContext& context) {
  // The previous buffer's last draws were submitted with the previous frame
  // and won't be repeated (this frame draws the new buffer), so hand the
  // previous buffer back to the producer now.
  SignalPendingReleaseAck();

  // Wait for the producer's writes before sampling. Olym's stills pass -1;
  // video passes a real sync_fd.
  if (direct.acquire_fence_fd >= 0) {
    WaitOnAndCloseSyncFd(direct.acquire_fence_fd);
    direct.acquire_fence_fd = -1;
  }

  // Defer this buffer's ack to the next frame (or destruction). Zeroing the
  // handle's copy keeps the cleanup below from signaling it early.
  pending_release_ack_fd_ = direct.release_ack_fd;
  direct.release_ack_fd = -1;

  // Drop the transferred AHB reference on every exit. The EGLImage made on
  // a cache miss holds its own reference, like the Vulkan texture source.
  fml::ScopedCleanupClosure cleanup(
      [this, direct]() { ReleaseAcquiredHardwareBuffer(direct); });

  BufferInfo info;
  info.color_space = direct.color_space;
  auto desc = impeller::android::HardwareBuffer::Describe(direct.buffer);
  if (desc.has_value()) {
    info.size = impeller::ISize(desc->width, desc->height);
    info.format = desc->format;
  }
  UpdateImage(direct.buffer, bounds, context, info);
}

void ImageExternalTextureGL::SignalPendingReleaseAck() {
  if (pending_release_ack_fd_ < 0) {
    return;
  }
  // Signaling early would let the producer overwrite a buffer the GPU may
  // still be reading, so wait for the submitted work first.
  WaitForSubmittedGpuWork();
  SignalAndCloseAckFd(pending_release_ack_fd_);
  pending_release_ack_fd_ = -1;
}

void ImageExternalTextureGL::Detach() {
  image_lru_.Clear();
  gl_entries_.clear();
}

impeller::UniqueEGLImageKHR ImageExternalTextureGL::CreateEGLImage(
    AHardwareBuffer* hardware_buffer) {
  if (hardware_buffer == nullptr) {
    return impeller::UniqueEGLImageKHR();
  }

  EGLDisplay display = eglGetCurrentDisplay();
  if (display == EGL_NO_DISPLAY) {
    // This could happen when running in a deferred task that executes after
    // the thread has lost its EGL state.
    return impeller::UniqueEGLImageKHR();
  }

  EGLClientBuffer client_buffer =
      impeller::android::GetProcTable().eglGetNativeClientBufferANDROID(
          hardware_buffer);
  FML_DCHECK(client_buffer != nullptr);
  if (client_buffer == nullptr) {
    FML_LOG(ERROR) << "eglGetNativeClientBufferAndroid returned null.";
    return impeller::UniqueEGLImageKHR();
  }

  impeller::EGLImageKHRWithDisplay maybe_image =
      impeller::EGLImageKHRWithDisplay{
          eglCreateImageKHR(display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                            client_buffer, 0),
          display};

  return impeller::UniqueEGLImageKHR(maybe_image);
}

}  // namespace flutter
