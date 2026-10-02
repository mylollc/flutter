// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/android/image_external_texture_vk_impeller.h"

#include <cstdint>
#include <memory>

#include "flutter/fml/closure.h"
#include "flutter/fml/logging.h"
#include "flutter/impeller/core/formats.h"
#include "flutter/impeller/core/texture_descriptor.h"
#include "flutter/impeller/display_list/dl_image_impeller.h"
#include "flutter/impeller/renderer/backend/vulkan/android/ahb_texture_source_vk.h"
#include "flutter/impeller/renderer/backend/vulkan/capabilities_vk.h"
#include "flutter/impeller/renderer/backend/vulkan/command_buffer_vk.h"
#include "flutter/impeller/renderer/backend/vulkan/queue_vk.h"
#include "flutter/impeller/renderer/backend/vulkan/texture_vk.h"
#include "flutter/impeller/toolkit/android/hardware_buffer.h"

namespace flutter {

namespace {

namespace vk = impeller::vk;

// Bound on waiting, at teardown, for release work still on the GPU.
constexpr uint64_t kTeardownWaitTimeoutNs = 2'000'000'000;

constexpr vk::ImageSubresourceRange kColorSubresource = {
    vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};

// Makes release fences on Impeller's graphics queue, to which every frame's
// draws are submitted: a signal operation covers all earlier submissions on
// its queue. A release also hands each released image back to the foreign
// queue family its producer writes from.
class VKReleaseFenceMaker final : public ReleaseFenceMaker {
 public:
  explicit VKReleaseFenceMaker(std::shared_ptr<impeller::ContextVK> context)
      : context_(std::move(context)) {}

  ~VKReleaseFenceMaker() override {
    // Destroying the objects of a pending submission is invalid.
    for (const std::shared_ptr<Submission>& submission : in_flight_) {
      if (context_->GetDevice().waitForFences(submission->fence.get(), true,
                                              kTeardownWaitTimeoutNs) !=
          vk::Result::eSuccess) {
        FML_LOG(ERROR) << "Release fence still pending at teardown.";
      }
    }
  }

  ReleaseFence Create(const std::vector<std::shared_ptr<void>>& images,
                      bool want_fence) override {
    ReapCompleted();
    const vk::Device& device = context_->GetDevice();
    auto submission = std::make_shared<Submission>();
    vk::SubmitInfo submit_info;

    std::vector<vk::ImageMemoryBarrier> barriers;
    for (const std::shared_ptr<void>& image : images) {
      if (image == nullptr) {
        continue;
      }
      auto texture = std::static_pointer_cast<impeller::TextureVK>(image);
      vk::ImageMemoryBarrier barrier;
      barrier.srcAccessMask = vk::AccessFlagBits::eShaderRead;
      barrier.oldLayout = texture->GetLayout();
      barrier.newLayout = texture->GetLayout();
      barrier.srcQueueFamilyIndex =
          context_->GetGraphicsQueue()->GetIndex().family;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
      barrier.image = texture->GetImage();
      barrier.subresourceRange = kColorSubresource;
      barriers.push_back(barrier);
      submission->images.push_back(std::move(texture));
    }
    if (!barriers.empty()) {
      submission->command_buffer = context_->CreateCommandBuffer();
      auto& command_buffer =
          impeller::CommandBufferVK::Cast(*submission->command_buffer);
      command_buffer.GetCommandBuffer().pipelineBarrier(
          vk::PipelineStageFlagBits::eAllCommands,
          vk::PipelineStageFlagBits::eBottomOfPipe, {}, nullptr, nullptr,
          barriers);
      if (!command_buffer.EndCommandBuffer()) {
        FML_LOG(ERROR) << "Release: could not record the ownership release.";
        return {};
      }
      submission->raw_command_buffer = command_buffer.GetCommandBuffer();
      submit_info.setCommandBuffers(submission->raw_command_buffer);
    } else if (!want_fence) {
      return {};
    }

    auto [fence_result, fence] = device.createFenceUnique({});
    if (fence_result != vk::Result::eSuccess) {
      FML_LOG(ERROR) << "Release: could not create a fence: "
                     << vk::to_string(fence_result);
      return {};
    }
    submission->fence = std::move(fence);

    const auto& capabilities =
        impeller::CapabilitiesVK::Cast(*context_->GetCapabilities());
    if (want_fence && capabilities.SupportsExternalSemaphoreExtensions()) {
      vk::StructureChain<vk::SemaphoreCreateInfo,
                         vk::ExportSemaphoreCreateInfoKHR>
          info;
      info.get<vk::ExportSemaphoreCreateInfoKHR>().handleTypes =
          vk::ExternalSemaphoreHandleTypeFlagBits::eSyncFd;
      auto [semaphore_result, semaphore] =
          device.createSemaphoreUnique(info.get());
      if (semaphore_result == vk::Result::eSuccess) {
        submission->semaphore = std::move(semaphore);
        submit_info.setSignalSemaphores(submission->semaphore.get());
      }
    }

    const vk::Result submit_result = context_->GetGraphicsQueue()->Submit(
        submit_info, submission->fence.get());
    if (submit_result != vk::Result::eSuccess) {
      FML_LOG(ERROR) << "Release: submit failed: "
                     << vk::to_string(submit_result);
      return {};
    }
    in_flight_.push_back(submission);
    if (!want_fence) {
      return {};
    }

    if (submission->semaphore) {
      vk::SemaphoreGetFdInfoKHR get_fd_info;
      get_fd_info.semaphore = submission->semaphore.get();
      get_fd_info.handleType = vk::ExternalSemaphoreHandleTypeFlagBits::eSyncFd;
      auto [fd_result, fd] = device.getSemaphoreFdKHR(get_fd_info);
      if (fd_result == vk::Result::eSuccess) {
        // -1 is a valid export: the work had already finished.
        return {.fd = fd};
      }
    }

    // No fence to hand over: check the submission's fence until it signals.
    return {.is_done = [device, submission]() {
      return device.getFenceStatus(submission->fence.get()) ==
             vk::Result::eSuccess;
    }};
  }

 private:
  // A release submission and everything it uses until its fence signals.
  struct Submission {
    vk::UniqueFence fence;
    vk::UniqueSemaphore semaphore;
    std::shared_ptr<impeller::CommandBuffer> command_buffer;
    vk::CommandBuffer raw_command_buffer;
    std::vector<std::shared_ptr<impeller::TextureVK>> images;
  };

  void ReapCompleted() {
    const vk::Device& device = context_->GetDevice();
    std::erase_if(in_flight_, [&device](const auto& submission) {
      return device.getFenceStatus(submission->fence.get()) ==
             vk::Result::eSuccess;
    });
  }

  const std::shared_ptr<impeller::ContextVK> context_;
  std::vector<std::shared_ptr<Submission>> in_flight_;
};

}  // namespace

ImageExternalTextureVKImpeller::ImageExternalTextureVKImpeller(
    const std::shared_ptr<impeller::ContextVK>& impeller_context,
    int64_t id,
    const fml::jni::ScopedJavaGlobalRef<jobject>& image_texture_entry,
    const std::shared_ptr<PlatformViewAndroidJNI>& jni_facade,
    ImageExternalTexture::ImageLifecycle lifecycle,
    fml::RefPtr<fml::TaskRunner> raster_task_runner)
    : ImageExternalTexture(id,
                           image_texture_entry,
                           jni_facade,
                           lifecycle,
                           std::move(raster_task_runner)),
      impeller_context_(impeller_context) {}

ImageExternalTextureVKImpeller::~ImageExternalTextureVKImpeller() {
  SetSampledHardwareBuffer(nullptr);
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
    WaitOnAndCloseSyncFd(direct.acquire_fence_fd);
    direct.acquire_fence_fd = -1;

    // RAII: drop the transferred AHB reference on every exit. The texture
    // source holds its own reference for as long as Impeller samples it.
    fml::ScopedCleanupClosure cleanup(
        [this, direct]() { ReleaseAcquiredHardwareBuffer(direct); });
    // A buffer pushed again while on screen is still owned here; any other
    // is acquired from its producer's (foreign) queue family.
    const bool acquire_ownership = direct.buffer != sampled_hardware_buffer();
    auto texture = IngestHardwareBuffer(direct.buffer, direct.color_space,
                                        acquire_ownership);
    if (texture) {
      SetSampledHardwareBuffer(direct.buffer, std::move(texture),
                               direct.wants_release);
    } else {
      // Never sampled; the previous buffer stays on screen.
      ReleaseUnsampled(direct.buffer);
    }
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
  if (IngestHardwareBuffer(AHardwareBufferFor(hardware_buffer),
                           /*color_space=*/-1,
                           /*acquire_ownership=*/false)) {
    SetSampledHardwareBuffer(nullptr);
  }
}

std::shared_ptr<impeller::TextureVK>
ImageExternalTextureVKImpeller::IngestHardwareBuffer(AHardwareBuffer* ahb,
                                                     int color_space,
                                                     bool acquire_ownership) {
  // Describe + LRU lookup is identical for both intake paths — the texture
  // source doesn't care how the raw AHB was obtained.
  auto hb_desc = impeller::android::HardwareBuffer::Describe(ahb);
  std::optional<HardwareBufferKey> key =
      impeller::android::HardwareBuffer::GetSystemUniqueID(ahb);
  auto existing_image = image_lru_.FindImage(key);
  if (!hb_desc.has_value()) {
    FML_LOG(ERROR) << "IngestHardwareBuffer (texture id=" << Id()
                   << "): HardwareBuffer::Describe returned no value; "
                      "dropping frame";
    return nullptr;
  }

  std::shared_ptr<impeller::TextureVK> texture;
  if (existing_image != nullptr) {
    // Cache hit: reuse the previously-constructed VkImage (same backing
    // AHB memory, already transitioned to SHADER_READ_ONLY_OPTIMAL). The
    // producer's new writes still need a fresh barrier, submitted below.
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
                     << "): AHBTextureSourceVK invalid; import failed "
                        "(check Vulkan validation layers).";
      return nullptr;
    }
    texture = std::make_shared<impeller::TextureVK>(impeller_context_,
                                                    texture_source);
  }

  auto buffer = impeller_context_->CreateCommandBuffer();
  impeller::CommandBufferVK& buffer_vk =
      impeller::CommandBufferVK::Cast(*buffer);

  if (acquire_ownership) {
    // Take the image from the producer's queue family. The contents of an
    // AHardwareBuffer acquired from the foreign queue family survive the
    // transition from UNDEFINED.
    vk::ImageMemoryBarrier barrier;
    barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
    barrier.oldLayout = vk::ImageLayout::eUndefined;
    barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    barrier.dstQueueFamilyIndex =
        impeller_context_->GetGraphicsQueue()->GetIndex().family;
    barrier.image = texture->GetImage();
    barrier.subresourceRange = kColorSubresource;
    buffer_vk.GetCommandBuffer().pipelineBarrier(
        vk::PipelineStageFlagBits::eTopOfPipe,
        vk::PipelineStageFlagBits::eFragmentShader, {}, nullptr, nullptr,
        barrier);
    texture->SetLayoutWithoutEncoding(vk::ImageLayout::eShaderReadOnlyOptimal);
  } else {
    // Transition the layout to shader read. On cache hit the image is
    // already in SHADER_READ_ONLY_OPTIMAL and Impeller's SetLayout call
    // is effectively a no-op.
    impeller::BarrierVK barrier;
    barrier.cmd_buffer = buffer_vk.GetCommandBuffer();
    barrier.src_access = vk::AccessFlagBits::eColorAttachmentWrite |
                         vk::AccessFlagBits::eTransferWrite;
    barrier.src_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput |
                        vk::PipelineStageFlagBits::eTransfer;
    barrier.dst_access = vk::AccessFlagBits::eShaderRead;
    barrier.dst_stage = vk::PipelineStageFlagBits::eFragmentShader;
    barrier.new_layout = vk::ImageLayout::eShaderReadOnlyOptimal;
    if (!texture->SetLayout(barrier)) {
      return nullptr;
    }
  }

  if (!impeller_context_->GetCommandQueue()
           ->Submit({buffer}, /*completion=*/nullptr,
                    /*block_on_schedule=*/false)
           .ok()) {
    return nullptr;
  }

  if (existing_image != nullptr) {
    dl_image_ = existing_image;
  } else {
    dl_image_ = impeller::DlImageImpeller::Make(texture);
    if (key.has_value()) {
      image_lru_.AddImage(dl_image_, key.value());
    }
  }
  return texture;
}

std::unique_ptr<ReleaseFenceMaker>
ImageExternalTextureVKImpeller::CreateReleaseFenceMaker() {
  return std::make_unique<VKReleaseFenceMaker>(impeller_context_);
}

}  // namespace flutter
