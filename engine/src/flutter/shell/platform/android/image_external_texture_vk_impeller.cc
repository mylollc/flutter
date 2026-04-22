// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/android/image_external_texture_vk_impeller.h"

#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>

#include "flutter/fml/closure.h"
#include "flutter/fml/logging.h"
#include "flutter/impeller/core/formats.h"
#include "flutter/impeller/core/texture_descriptor.h"
#include "flutter/impeller/display_list/dl_image_impeller.h"
#include "flutter/impeller/renderer/backend/vulkan/android/ahb_texture_source_vk.h"
#include "flutter/impeller/renderer/backend/vulkan/command_buffer_vk.h"
#include "flutter/impeller/renderer/backend/vulkan/texture_vk.h"
#include "flutter/impeller/toolkit/android/hardware_buffer.h"

namespace flutter {

namespace {

// Wait until a Linux sync_fd (POSIX fd emitted by
// `VK_KHR_external_semaphore_fd` in `SYNC_FD` mode) is signaled, then
// close it. Runs on the raster thread before the Ingest barrier submit
// — closes the producer→consumer cross-context sync gap that
// `VK_ERROR_DEVICE_LOST` under Mali would otherwise punish. `poll`
// with `POLLIN` is how the Android sync framework surfaces signal
// readiness on sync_fd; `sync_wait` would do the same via a slightly
// higher-level wrapper but isn't part of the NDK base surface we
// target. Infinite timeout is safe: a signaled sync_fd returns
// immediately, and an unsignaled one is waiting on GPU work that is
// guaranteed to complete (the producer has already submitted).
// Signal a producer-supplied `eventfd` (8-byte counter increment via
// `write`) and close it. Used on every exit path of IngestHardwareBuffer
// where the engine is done with the AHB — either because it sampled,
// because it can't sample (error), or because the sample finished
// asynchronously in an Impeller completion callback.
void SignalAndCloseAckFd(int fd) {
  if (fd < 0) {
    return;
  }
  const uint64_t one = 1;
  (void)::write(fd, &one, sizeof(one));
  ::close(fd);
}

// Producer-fence wait timeout. Normal signal latency is microseconds;
// this bound just keeps a stuck driver or hung producer from freezing
// the raster thread indefinitely. Exceeding it is unusual enough to
// warrant an error log; the sampling that follows may race producer
// GPU writes for one frame, typically self-correcting on the next.
constexpr int kSyncFenceTimeoutMs = 2000;

void WaitOnAndCloseSyncFd(int fd) {
  struct pollfd pfd;
  pfd.fd = fd;
  pfd.events = POLLIN;
  pfd.revents = 0;
  int rc;
  do {
    rc = ::poll(&pfd, 1, kSyncFenceTimeoutMs);
  } while (rc == -1 && errno == EINTR);
  if (rc < 0) {
    FML_LOG(ERROR) << "poll on sync_fd " << fd
                   << " failed: errno=" << errno
                   << " — sampling may race producer GPU writes";
  } else if (rc == 0) {
    FML_LOG(ERROR) << "poll on sync_fd " << fd << " timed out after "
                   << kSyncFenceTimeoutMs << "ms — producer fence never "
                   << "signaled; sampling may race GPU writes for this frame";
  }
  ::close(fd);
}

}  // namespace

ImageExternalTextureVKImpeller::ImageExternalTextureVKImpeller(
    const std::shared_ptr<impeller::ContextVK>& impeller_context,
    int64_t id,
    const fml::jni::ScopedJavaGlobalRef<jobject>& image_texture_entry,
    const std::shared_ptr<PlatformViewAndroidJNI>& jni_facade,
    ImageExternalTexture::ImageLifecycle lifecycle)
    : ImageExternalTexture(id, image_texture_entry, jni_facade, lifecycle),
      impeller_context_(impeller_context) {}

ImageExternalTextureVKImpeller::~ImageExternalTextureVKImpeller() {
  // Signal any deferred release-ack so the producer doesn't block
  // forever on its poll. By the time the external texture is
  // destroyed, any in-flight compositor work referencing its AHB has
  // long since completed (the engine owns the sequencing), so
  // signaling here is safe.
  SignalAndCloseAckFd(pending_release_ack_fd_);
  pending_release_ack_fd_ = -1;
}

void ImageExternalTextureVKImpeller::Attach(PaintContext& context) {
  if (state_ == AttachmentState::kUninitialized) {
    // First processed frame we are attached.
    state_ = AttachmentState::kAttached;
  }
}

void ImageExternalTextureVKImpeller::Detach() {}

void ImageExternalTextureVKImpeller::ProcessFrame(PaintContext& context,
                                                  const SkRect& bounds) {
  // Prefer the direct-AHB path: if a producer pushed a raw
  // `AHardwareBuffer` via `pushHardwareBuffer`, consume it without the
  // `Image`/`HardwareBuffer` wrapping overhead. Falling through to the
  // legacy `Image` path when no raw AHB is pending keeps backward
  // compatibility with producers that still push via `pushImage`.
  AcquiredHardwareBuffer direct = AcquireLatestHardwareBuffer();
  if (direct.buffer != nullptr) {
    // Consume the producer's completion fence *before* sampling so
    // Impeller's barrier transition + fragment read don't race the
    // producer's in-flight GPU writes on the shared AHardwareBuffer
    // (Mali drivers respond with VK_ERROR_DEVICE_LOST to that race).
    if (direct.acquire_fence_fd >= 0) {
      WaitOnAndCloseSyncFd(direct.acquire_fence_fd);
      direct.acquire_fence_fd = -1;
    }

    // Extract the release-ack fd from the handle and zero it so the
    // cleanup closure below doesn't double-signal. `IngestHardwareBuffer`
    // takes ownership of the fd and signals+closes on every exit path
    // (synchronously on cache hit / error, asynchronously via an
    // Impeller submit completion callback on the normal barrier path).
    int release_ack_fd = direct.release_ack_fd;
    direct.release_ack_fd = -1;

    // RAII: release AHB + remaining fence_fd on every exit. The
    // release-ack fd is handled by `IngestHardwareBuffer`.
    fml::ScopedCleanupClosure cleanup(
        [this, direct]() { ReleaseAcquiredHardwareBuffer(direct); });
    IngestHardwareBuffer(direct.buffer, release_ack_fd, direct.color_space);
    return;
  }

  JavaLocalRef image = AcquireLatestImage();
  if (image.is_null()) {
    return;
  }
  JavaLocalRef hardware_buffer = HardwareBufferFor(image);
  // RAII cleanup of the Java `HardwareBuffer` wrapper. The underlying
  // `AHardwareBuffer` stays alive because Impeller's texture source holds
  // its own reference.
  fml::ScopedCleanupClosure cleanup(
      [this, &hardware_buffer]() { CloseHardwareBuffer(hardware_buffer); });
  // Image-based intake path has no release-ack fd; the producer drove
  // pushImage (CPU-synchronous ImageReader), not pushHardwareBuffer.
  IngestHardwareBuffer(AHardwareBufferFor(hardware_buffer),
                       /*release_ack_fd=*/-1,
                       /*color_space=*/-1);
}

void ImageExternalTextureVKImpeller::IngestHardwareBuffer(AHardwareBuffer* ahb,
                                                          int release_ack_fd,
                                                          int color_space) {
  // One-frame-deferred release-ack: signal the *previous* frame's
  // ack fd now, stash THIS frame's at the end of the function. The
  // deferral gives the compositor a cycle to drain its sampling of
  // the prior AHB before the producer is told the buffer is free to
  // reuse. A tighter binding — signaling on the actual composite-
  // completion fence — would need an Impeller-side hook we don't
  // have today; the current deferral is safe as long as the producer
  // tolerates a one-frame reuse latency (which any realistic video /
  // ring-buffer pipeline will).
  SignalAndCloseAckFd(pending_release_ack_fd_);
  pending_release_ack_fd_ = -1;

  // RAII signal+close for THIS frame's ack fd on early return paths
  // (describe failure, texture-source invalid, submit failure). On the
  // normal path we transfer ownership to `pending_release_ack_fd_` at
  // the very end, so the next Ingest signals it.
  fml::ScopedCleanupClosure ack_cleanup(
      [&release_ack_fd]() { SignalAndCloseAckFd(release_ack_fd); });

  // Describe + LRU lookup is identical for both intake paths — the texture
  // source doesn't care how the raw AHB was obtained.
  auto hb_desc = impeller::android::HardwareBuffer::Describe(ahb);
  std::optional<HardwareBufferKey> key =
      impeller::android::HardwareBuffer::GetSystemUniqueID(ahb);
  auto existing_image = image_lru_.FindImage(key);
  if (!hb_desc.has_value()) {
    FML_LOG(ERROR) << "IngestHardwareBuffer (texture id=" << Id()
                   << "): HardwareBuffer::Describe returned no value — "
                      "dropping frame";
    return;
  }

  std::shared_ptr<impeller::TextureVK> texture;
  if (existing_image != nullptr) {
    // Cache hit: reuse the previously-constructed VkImage (same backing
    // AHB memory, already transitioned to SHADER_READ_ONLY_OPTIMAL). We
    // still need a fresh sync point for the producer's new writes, so
    // we fall through to submit a (near-no-op) barrier below that just
    // signals the completion callback after any prior commands drain.
    dl_image_ = existing_image;
    auto impeller_tex = existing_image->impeller_texture();
    if (impeller_tex) {
      texture = std::static_pointer_cast<impeller::TextureVK>(impeller_tex);
    }
  }

  if (texture == nullptr) {
    auto texture_source = std::make_shared<impeller::AHBTextureSourceVK>(
        impeller_context_, ahb, hb_desc.value(), color_space);
    if (!texture_source->IsValid()) {
      FML_LOG(ERROR) << "IngestHardwareBuffer (texture id=" << Id()
                     << "): AHBTextureSourceVK invalid — import failed "
                        "(check Vulkan validation layers).";
      return;
    }
    texture = std::make_shared<impeller::TextureVK>(impeller_context_,
                                                    texture_source);
  }

  // Transition the layout to shader read. On cache hit the image is
  // already in SHADER_READ_ONLY_OPTIMAL and Impeller's SetLayout call
  // is effectively a no-op; the command-queue submit below may also be
  // a no-op in that case. Release-ack signaling is NOT tied to a
  // submit-completion callback — the submit passes `completion=nullptr`
  // below. Instead, ack is deferred: this frame's `release_ack_fd` is
  // stashed into `pending_release_ack_fd_` at the end of this function,
  // and signaled on the *next* Ingest (or in Detach/destructor). That
  // one-frame deferral gives the compositor a cycle to finish sampling
  // the prior AHB before the producer is told the slot is free.
  auto buffer = impeller_context_->CreateCommandBuffer();
  impeller::CommandBufferVK& buffer_vk =
      impeller::CommandBufferVK::Cast(*buffer);

  impeller::BarrierVK barrier;
  barrier.cmd_buffer = buffer_vk.GetCommandBuffer();
  barrier.src_access = impeller::vk::AccessFlagBits::eColorAttachmentWrite |
                       impeller::vk::AccessFlagBits::eTransferWrite;
  barrier.src_stage =
      impeller::vk::PipelineStageFlagBits::eColorAttachmentOutput |
      impeller::vk::PipelineStageFlagBits::eTransfer;
  barrier.dst_access = impeller::vk::AccessFlagBits::eShaderRead;
  barrier.dst_stage = impeller::vk::PipelineStageFlagBits::eFragmentShader;
  barrier.new_layout = impeller::vk::ImageLayout::eShaderReadOnlyOptimal;

  if (!texture->SetLayout(barrier)) {
    return;
  }

  if (!impeller_context_
           ->GetCommandQueue()
           ->Submit({buffer}, /*completion=*/nullptr, /*block_on_schedule=*/false)
           .ok()) {
    // Submit failure: RAII signals the ack fd immediately. Nothing to
    // defer since the compositor never got the frame.
    return;
  }

  dl_image_ = impeller::DlImageImpeller::Make(texture);
  if (key.has_value() && existing_image == nullptr) {
    image_lru_.AddImage(dl_image_, key.value());
  }

  // Defer the ack: stash this frame's fd in `pending_release_ack_fd_`
  // and disarm the RAII. The next Ingest (or Detach/destructor) will
  // signal it. Rationale at the top of IngestHardwareBuffer.
  pending_release_ack_fd_ = release_ack_fd;
  release_ack_fd = -1;
}

}  // namespace flutter
