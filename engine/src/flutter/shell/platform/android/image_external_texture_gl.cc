// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/android/image_external_texture_gl.h"

#include <android/hardware_buffer_jni.h>
#include <android/sensor.h>

#include <cstring>

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

using DupNativeFenceFDProc = EGLint (*)(EGLDisplay, EGLSyncKHR);
using WaitSyncProc = EGLint (*)(EGLDisplay, EGLSyncKHR, EGLint);

// `eglDupNativeFenceFDANDROID` is missing from the NDK's libEGL stubs (and
// `eglWaitSyncKHR` from older ones), so both are resolved at runtime.
DupNativeFenceFDProc GetDupNativeFenceFDProc() {
  static const DupNativeFenceFDProc proc =
      reinterpret_cast<DupNativeFenceFDProc>(
          eglGetProcAddress("eglDupNativeFenceFDANDROID"));
  return proc;
}

WaitSyncProc GetWaitSyncProc() {
  static const WaitSyncProc proc =
      reinterpret_cast<WaitSyncProc>(eglGetProcAddress("eglWaitSyncKHR"));
  return proc;
}

// Whether `display` can import and export native (sync fd) fences.
bool SupportsNativeFenceSync(EGLDisplay display) {
  static EGLDisplay checked_display = EGL_NO_DISPLAY;
  static bool supported = false;
  if (display != checked_display) {
    const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
    supported =
        extensions != nullptr &&
        std::strstr(extensions, "EGL_ANDROID_native_fence_sync") != nullptr &&
        std::strstr(extensions, "EGL_KHR_wait_sync") != nullptr &&
        GetDupNativeFenceFDProc() != nullptr && GetWaitSyncProc() != nullptr;
    checked_display = display;
  }
  return supported;
}

// Makes release fences on the raster thread's current (onscreen) context,
// on which every frame's draws are issued.
class GLReleaseFenceMaker final : public ReleaseFenceMaker {
 public:
  ReleaseFence Create(const std::vector<std::shared_ptr<void>>& images,
                      bool want_fence) override {
    if (!want_fence) {
      return {};
    }
    EGLDisplay display = eglGetCurrentDisplay();
    if (display == EGL_NO_DISPLAY || eglGetCurrentContext() == EGL_NO_CONTEXT) {
      // Only during teardown: the frames that drew the buffers were issued
      // on a context that is gone, so their work is complete.
      return {};
    }

    if (SupportsNativeFenceSync(display)) {
      const EGLint attributes[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID,
                                   EGL_NO_NATIVE_FENCE_FD_ANDROID, EGL_NONE};
      EGLSyncKHR sync =
          eglCreateSyncKHR(display, EGL_SYNC_NATIVE_FENCE_ANDROID, attributes);
      if (sync != EGL_NO_SYNC_KHR) {
        // The fence's fd only exists once the fence command is flushed.
        glFlush();
        const EGLint fd = GetDupNativeFenceFDProc()(display, sync);
        eglDestroySyncKHR(display, sync);
        if (fd != EGL_NO_NATIVE_FENCE_FD_ANDROID) {
          return {.fd = fd};
        }
      }
    }

    // No fence to hand over: check an EGL fence until it signals.
    EGLSyncKHR sync = eglCreateSyncKHR(display, EGL_SYNC_FENCE_KHR, nullptr);
    if (sync == EGL_NO_SYNC_KHR) {
      // No fences at all (EGL_KHR_fence_sync is near universal).
      glFinish();
      return {};
    }
    glFlush();
    std::shared_ptr<void> fence(
        sync, [display](void* sync) { eglDestroySyncKHR(display, sync); });
    return {.is_done = [display, fence]() {
      return eglClientWaitSyncKHR(display, fence.get(), 0, 0) !=
             EGL_TIMEOUT_EXPIRED_KHR;
    }};
  }
};

// Make this context's later commands wait, on the GPU, for a producer's
// acquire fence. Takes ownership of `fd`.
void WaitForAcquireFence(int fd) {
  if (fd < 0) {
    return;
  }
  EGLDisplay display = eglGetCurrentDisplay();
  if (display != EGL_NO_DISPLAY && SupportsNativeFenceSync(display)) {
    const EGLint attributes[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fd,
                                 EGL_NONE};
    EGLSyncKHR sync =
        eglCreateSyncKHR(display, EGL_SYNC_NATIVE_FENCE_ANDROID, attributes);
    if (sync != EGL_NO_SYNC_KHR) {
      // The sync owns `fd` now.
      if (GetWaitSyncProc()(display, sync, 0) != EGL_TRUE) {
        FML_LOG(ERROR) << "eglWaitSyncKHR failed; waiting on the CPU.";
        eglClientWaitSyncKHR(display, sync, 0, EGL_FOREVER_KHR);
      }
      eglDestroySyncKHR(display, sync);
      return;
    }
  }
  ImageExternalTexture::WaitOnAndCloseSyncFd(fd);
}

}  // namespace

ImageExternalTextureGL::ImageExternalTextureGL(
    int64_t id,
    const fml::jni::ScopedJavaGlobalRef<jobject>& image_texture_entry,
    const std::shared_ptr<PlatformViewAndroidJNI>& jni_facade,
    ImageExternalTexture::ImageLifecycle lifecycle,
    fml::RefPtr<fml::TaskRunner> raster_task_runner)
    : ImageExternalTexture(id,
                           image_texture_entry,
                           jni_facade,
                           lifecycle,
                           std::move(raster_task_runner)) {}

ImageExternalTextureGL::~ImageExternalTextureGL() {
  SetSampledHardwareBuffer(nullptr);
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

bool ImageExternalTextureGL::UpdateImage(JavaLocalRef& hardware_buffer,
                                         const SkRect& bounds,
                                         PaintContext& context) {
  return UpdateImage(AHardwareBufferFor(hardware_buffer), bounds, context,
                     BufferInfo{});
}

bool ImageExternalTextureGL::UpdateImage(AHardwareBuffer* hardware_buffer,
                                         const SkRect& bounds,
                                         PaintContext& context,
                                         const BufferInfo& info) {
  std::optional<HardwareBufferKey> key =
      impeller::android::HardwareBuffer::GetSystemUniqueID(hardware_buffer);
  auto existing_image = image_lru_.FindImage(key);
  if (existing_image != nullptr) {
    dl_image_ = existing_image;
    return true;
  }

  auto egl_image = CreateEGLImage(hardware_buffer);
  if (!egl_image.is_valid()) {
    return false;
  }

  auto image = CreateDlImage(context, bounds, key, std::move(egl_image), info);
  if (image == nullptr) {
    return false;
  }
  dl_image_ = std::move(image);
  if (key.has_value()) {
    gl_entries_.erase(image_lru_.AddImage(dl_image_, key.value()));
  }
  return true;
}

void ImageExternalTextureGL::ProcessFrame(PaintContext& context,
                                          const SkRect& bounds) {
  // Prefer the direct-AHB path: a producer on `pushHardwareBuffer` leaves the
  // `Image` slot empty. Falling through to the `Image` path when no raw AHB
  // is pending keeps producers that push via `pushImage` working.
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
  if (UpdateImage(hardware_buffer, bounds, context)) {
    SetSampledHardwareBuffer(nullptr);
  }
  CloseHardwareBuffer(hardware_buffer);
}

void ImageExternalTextureGL::IngestHardwareBuffer(AcquiredHardwareBuffer direct,
                                                  const SkRect& bounds,
                                                  PaintContext& context) {
  // Make this frame's draws wait for the producer's writes.
  WaitForAcquireFence(direct.acquire_fence_fd);
  direct.acquire_fence_fd = -1;

  // Drop the transferred AHB reference on every exit. The EGLImage made on a
  // cache miss holds its own reference.
  fml::ScopedCleanupClosure cleanup(
      [this, direct]() { ReleaseAcquiredHardwareBuffer(direct); });

  BufferInfo info;
  info.color_space = direct.color_space;
  auto desc = impeller::android::HardwareBuffer::Describe(direct.buffer);
  if (desc.has_value()) {
    info.size = impeller::ISize(desc->width, desc->height);
    info.format = desc->format;
  }
  if (UpdateImage(direct.buffer, bounds, context, info)) {
    SetSampledHardwareBuffer(direct.buffer, /*image=*/nullptr,
                             direct.wants_release);
  } else {
    // Never sampled; the previous buffer stays on screen.
    ReleaseUnsampled(direct.buffer);
  }
}

std::unique_ptr<ReleaseFenceMaker>
ImageExternalTextureGL::CreateReleaseFenceMaker() {
  return std::make_unique<GLReleaseFenceMaker>();
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
