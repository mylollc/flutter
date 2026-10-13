// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Release fences made by the real OpenGL ES and Vulkan backends. These need a
// GPU, so they run on a device or emulator.

#include <poll.h>
#include <unistd.h>

#include <memory>

#include "flutter/display_list/dl_builder.h"
#include "flutter/fml/native_library.h"
#include "flutter/fml/synchronization/waitable_event.h"
#include "flutter/fml/thread.h"
#include "flutter/impeller/renderer/backend/vulkan/context_vk.h"
#include "flutter/impeller/toolkit/android/hardware_buffer.h"
#include "flutter/impeller/toolkit/android/proc_table.h"
#include "flutter/impeller/toolkit/egl/config.h"
#include "flutter/impeller/toolkit/egl/context.h"
#include "flutter/impeller/toolkit/egl/display.h"
#include "flutter/impeller/toolkit/egl/surface.h"
#include "flutter/shell/platform/android/image_external_texture_gl.h"
#include "flutter/shell/platform/android/image_external_texture_vk_impeller.h"
#include "flutter/shell/platform/android/jni/jni_mock.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace flutter {
namespace testing {

using ::testing::_;
using ::testing::Invoke;

namespace {

constexpr int kFenceTimeoutMs = 2000;

// Whether `fd` signals within the timeout.
bool Signals(int fd) {
  struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
  return ::poll(&pfd, 1, kFenceTimeoutMs) == 1;
}

// Waits for a release fence to signal, or for its fallback check to pass.
bool Completes(ReleaseFence& fence) {
  if (fence.fd >= 0) {
    const bool signals = Signals(fence.fd);
    ::close(fence.fd);
    return signals;
  }
  if (!fence.is_done) {
    return true;
  }
  for (int i = 0; i < kFenceTimeoutMs; i++) {
    if (fence.is_done()) {
      return true;
    }
    ::usleep(1000);
  }
  return false;
}

// Exposes the release fence maker of the OpenGL ES texture.
class TestImageExternalTextureGL final : public ImageExternalTextureGL {
 public:
  explicit TestImageExternalTextureGL(
      const std::shared_ptr<PlatformViewAndroidJNI>& jni)
      : ImageExternalTextureGL(/*id=*/1,
                               fml::jni::ScopedJavaGlobalRef<jobject>(),
                               jni,
                               ImageLifecycle::kReset,
                               /*raster_task_runner=*/nullptr) {}

  std::unique_ptr<ReleaseFenceMaker> MakeReleaseFenceMaker() {
    return CreateReleaseFenceMaker();
  }

 private:
  sk_sp<flutter::DlImage> CreateDlImage(PaintContext& context,
                                        const SkRect& bounds,
                                        std::optional<HardwareBufferKey> id,
                                        impeller::UniqueEGLImageKHR&& egl_image,
                                        const BufferInfo& info) override {
    return nullptr;
  }
};

std::shared_ptr<impeller::ContextVK> CreateVulkanContext() {
  auto vulkan = fml::NativeLibrary::Create("libvulkan.so");
  if (!vulkan) {
    return nullptr;
  }
  auto get_instance_proc_address =
      vulkan->ResolveFunction<PFN_vkGetInstanceProcAddr>(
          "vkGetInstanceProcAddr");
  if (!get_instance_proc_address.has_value()) {
    return nullptr;
  }
  impeller::ContextVK::Settings settings;
  settings.proc_address_callback = get_instance_proc_address.value();
  settings.enable_validation = false;
  settings.enable_gpu_tracing = false;
  settings.enable_surface_control = false;
  return impeller::ContextVK::Create(std::move(settings));
}

std::unique_ptr<impeller::android::HardwareBuffer> CreateBuffer() {
  impeller::android::HardwareBufferDescriptor descriptor;
  descriptor.size = impeller::ISize{4, 4};
  descriptor.format =
      impeller::android::HardwareBufferFormat::kR8G8B8A8UNormInt;
  descriptor.usage = impeller::android::HardwareBufferUsageFlags::kSampledImage;
  auto buffer = std::make_unique<impeller::android::HardwareBuffer>(descriptor);
  return buffer->IsValid() ? std::move(buffer) : nullptr;
}

// What `AcquireLatestHardwareBuffer` hands the texture for a push of
// `buffer`: one reference for the engine, no acquire fence.
AcquiredHardwareBuffer Push(AHardwareBuffer* buffer) {
  impeller::android::GetProcTable().AHardwareBuffer_acquire(buffer);
  return AcquiredHardwareBuffer{.buffer = buffer};
}

// Runs `task` on `thread` and waits for it.
void RunOn(fml::Thread& thread, const fml::closure& task) {
  fml::AutoResetWaitableEvent latch;
  thread.GetTaskRunner()->PostTask([&task, &latch]() {
    task();
    latch.Signal();
  });
  latch.Wait();
}

}  // namespace

TEST(ImageExternalTextureGLGpuTest, ReleaseFenceSignalsAfterSubmittedWork) {
  impeller::egl::Display display;
  ASSERT_TRUE(display.IsValid());
  impeller::egl::ConfigDescriptor descriptor;
  descriptor.api = impeller::egl::API::kOpenGLES3;
  descriptor.color_format = impeller::egl::ColorFormat::kRGBA8888;
  descriptor.surface_type = impeller::egl::SurfaceType::kPBuffer;
  auto config = display.ChooseConfig(descriptor);
  if (!config) {
    descriptor.api = impeller::egl::API::kOpenGLES2;
    config = display.ChooseConfig(descriptor);
  }
  ASSERT_TRUE(config);
  auto context = display.CreateContext(*config, nullptr);
  ASSERT_TRUE(context);
  auto surface = display.CreatePixelBufferSurface(*config, 1, 1);
  ASSERT_TRUE(surface);
  ASSERT_TRUE(context->MakeCurrent(*surface));

  auto jni = std::make_shared<JNIMock>();
  TestImageExternalTextureGL texture(jni);
  auto maker = texture.MakeReleaseFenceMaker();

  // Some GPU work for the fence to follow.
  glClearColor(1, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  ReleaseFence fence = maker->Create({}, /*want_fence=*/true);
  EXPECT_TRUE(fence.fd >= 0 || fence.is_done);
  EXPECT_TRUE(Completes(fence));

  // Without a listener nothing is made.
  ReleaseFence none = maker->Create({}, /*want_fence=*/false);
  EXPECT_EQ(none.fd, -1);
  EXPECT_FALSE(none.is_done);

  EXPECT_TRUE(context->ClearCurrent());
}

TEST(ImageExternalTextureVKGpuTest, ReplacedBufferComesBackBehindItsFence) {
  if (!impeller::android::HardwareBuffer::IsAvailableOnPlatform()) {
    GTEST_SKIP() << "AHardwareBuffer is unavailable.";
  }
  auto context = CreateVulkanContext();
  if (!context) {
    GTEST_SKIP() << "No Vulkan device.";
  }
  auto buffer_a = CreateBuffer();
  auto buffer_b = CreateBuffer();
  ASSERT_TRUE(buffer_a && buffer_b);
  AHardwareBuffer* ahb_a = buffer_a->GetHandle();
  AHardwareBuffer* ahb_b = buffer_b->GetHandle();

  auto jni = std::make_shared<JNIMock>();
  EXPECT_CALL(*jni, ImageProducerTextureEntryAcquireLatestHardwareBuffer(_))
      .WillOnce(Invoke([ahb_a](auto) { return Push(ahb_a); }))
      .WillOnce(Invoke([ahb_b](auto) { return Push(ahb_b); }));

  int release_fence = -2;
  fml::AutoResetWaitableEvent released;
  EXPECT_CALL(*jni,
              ImageProducerTextureEntryOnHardwareBufferReleased(_, ahb_a, _))
      .WillOnce(Invoke([&](auto, auto, int fence_fd) {
        release_fence = fence_fd;
        released.Signal();
      }));

  fml::Thread raster("raster");
  std::shared_ptr<ImageExternalTextureVKImpeller> texture;
  RunOn(raster, [&]() {
    texture = std::make_shared<ImageExternalTextureVKImpeller>(
        context, /*id=*/1, fml::jni::ScopedJavaGlobalRef<jobject>(), jni,
        ImageExternalTexture::ImageLifecycle::kReset, raster.GetTaskRunner());
  });

  // Two frames: the first draws A, the second replaces it with B.
  for (int frame = 0; frame < 2; frame++) {
    RunOn(raster, [&]() {
      DisplayListBuilder builder;
      Texture::PaintContext paint_context = {.canvas = &builder};
      static_cast<Texture&>(*texture).Paint(paint_context, DlRect::MakeWH(4, 4),
                                            /*freeze=*/false,
                                            DlImageSampling::kLinear);
    });
  }

  released.Wait();
  // A sync fd, or -1 when the work had already finished.
  ASSERT_GE(release_fence, -1);
  if (release_fence >= 0) {
    EXPECT_TRUE(Signals(release_fence));
    ::close(release_fence);
  }

  // B is still on screen; it goes back when the texture does.
  EXPECT_CALL(*jni,
              ImageProducerTextureEntryOnHardwareBufferReleased(_, ahb_b, _))
      .WillOnce(Invoke([](auto, auto, int fence_fd) {
        if (fence_fd >= 0) {
          ::close(fence_fd);
        }
      }));
  RunOn(raster, [&]() { texture.reset(); });
  RunOn(raster, []() {});
  context->Shutdown();
}

}  // namespace testing
}  // namespace flutter
