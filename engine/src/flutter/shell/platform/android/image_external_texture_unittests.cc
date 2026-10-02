// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/android/image_external_texture.h"

#include <unistd.h>

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "flutter/fml/synchronization/waitable_event.h"
#include "flutter/fml/thread.h"
#include "flutter/shell/platform/android/jni/jni_mock.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace flutter {
namespace testing {

using ::testing::_;
using ::testing::Ge;
using ::testing::Invoke;

namespace {

// Stand-ins for producer buffers. The release bookkeeping only compares and
// forwards the pointers, never dereferences them.
AHardwareBuffer* const kBufferA = reinterpret_cast<AHardwareBuffer*>(0x1000);
AHardwareBuffer* const kBufferB = reinterpret_cast<AHardwareBuffer*>(0x2000);
AHardwareBuffer* const kBufferC = reinterpret_cast<AHardwareBuffer*>(0x3000);

// What the fake backend was asked for.
struct FenceRequests {
  std::vector<std::vector<std::shared_ptr<void>>> images;
  std::vector<bool> want_fence;
  // When false, fences can't be exported and releases wait for `done`.
  bool exportable = true;
  bool done = false;
};

class FakeReleaseFenceMaker final : public ReleaseFenceMaker {
 public:
  explicit FakeReleaseFenceMaker(std::shared_ptr<FenceRequests> requests)
      : requests_(std::move(requests)) {}

  ReleaseFence Create(const std::vector<std::shared_ptr<void>>& images,
                      bool want_fence) override {
    requests_->images.push_back(images);
    requests_->want_fence.push_back(want_fence);
    if (!want_fence) {
      return {};
    }
    if (!requests_->exportable) {
      return {.is_done = [requests = requests_]() { return requests->done; }};
    }
    // A real fd, so that releases can duplicate and close it.
    int fds[2];
    FML_CHECK(::pipe(fds) == 0);
    ::close(fds[1]);
    return {.fd = fds[0]};
  }

 private:
  std::shared_ptr<FenceRequests> requests_;
};

// Exercises the backend-independent release bookkeeping of
// ImageExternalTexture with a fake backend.
class TestImageExternalTexture : public ImageExternalTexture {
 public:
  TestImageExternalTexture(const std::shared_ptr<PlatformViewAndroidJNI>& jni,
                           ImageLifecycle lifecycle,
                           fml::RefPtr<fml::TaskRunner> raster_task_runner)
      : ImageExternalTexture(/*id=*/1,
                             fml::jni::ScopedJavaGlobalRef<jobject>(),
                             jni,
                             lifecycle,
                             std::move(raster_task_runner)) {}

  ~TestImageExternalTexture() override = default;

  // Draw `buffer` (nullptr: a frame from the Image flow) from now on.
  void Sample(AHardwareBuffer* buffer,
              std::shared_ptr<void> image = nullptr,
              bool wants_release = true) {
    SetSampledHardwareBuffer(buffer, std::move(image), wants_release);
  }

  void DropUnsampled(AHardwareBuffer* buffer) { ReleaseUnsampled(buffer); }

  void MarkAttached() { state_ = AttachmentState::kAttached; }

  const std::shared_ptr<FenceRequests> requests =
      std::make_shared<FenceRequests>();

 private:
  void ProcessFrame(PaintContext& context, const SkRect& bounds) override {}
  void Attach(PaintContext& context) override {}
  void Detach() override {}

  std::unique_ptr<ReleaseFenceMaker> CreateReleaseFenceMaker() override {
    return std::make_unique<FakeReleaseFenceMaker>(requests);
  }
};

// The mock takes ownership of release fences; close them.
void CloseFence(const JavaLocalRef&, AHardwareBuffer*, int fence_fd) {
  if (fence_fd >= 0) {
    ::close(fence_fd);
  }
}

// Each test drives a texture on a raster thread. A task run there plays the
// part of a frame: the releases it causes happen in tasks posted after it.
class ImageExternalTextureTest : public ::testing::Test {
 protected:
  std::unique_ptr<TestImageExternalTexture> MakeTexture(
      ImageExternalTexture::ImageLifecycle lifecycle =
          ImageExternalTexture::ImageLifecycle::kReset) {
    return std::make_unique<TestImageExternalTexture>(
        jni_, lifecycle, raster_thread_.GetTaskRunner());
  }

  // Runs `frame` on the raster thread and waits for it.
  void RunFrame(const fml::closure& frame) {
    fml::AutoResetWaitableEvent latch;
    raster_thread_.GetTaskRunner()->PostTask([&frame, &latch]() {
      frame();
      latch.Signal();
    });
    latch.Wait();
  }

  // Waits for the tasks the last frame posted.
  void FinishFrame() {
    RunFrame([]() {});
  }

  fml::Thread raster_thread_{"raster"};
  std::shared_ptr<JNIMock> jni_ = std::make_shared<JNIMock>();
};

}  // namespace

TEST_F(ImageExternalTextureTest, FirstSampledBufferReleasesNothing) {
  auto texture = MakeTexture();
  EXPECT_CALL(*jni_, ImageProducerTextureEntryOnHardwareBufferReleased(_, _, _))
      .Times(0);

  RunFrame([&]() { texture->Sample(kBufferA); });
  FinishFrame();

  EXPECT_TRUE(texture->requests->images.empty());
}

TEST_F(ImageExternalTextureTest, ReplacedBufferIsReleasedAfterTheFrame) {
  auto texture = MakeTexture();
  EXPECT_CALL(*jni_, ImageProducerTextureEntryOnHardwareBufferReleased(
                         _, kBufferA, Ge(0)))
      .WillOnce(Invoke(CloseFence));

  RunFrame([&]() {
    texture->Sample(kBufferA);
    texture->Sample(kBufferB);
    // Nothing goes back while the frame that may still draw A is open.
    EXPECT_TRUE(texture->requests->images.empty());
  });
  FinishFrame();

  ASSERT_EQ(texture->requests->want_fence.size(), 1u);
  EXPECT_TRUE(texture->requests->want_fence[0]);
}

TEST_F(ImageExternalTextureTest, BuffersReplacedInOneFrameShareOneFence) {
  auto texture = MakeTexture();

  // A texture painted twice in a frame can replace two buffers in it; both
  // go back behind a fence made after the whole frame was submitted.
  EXPECT_CALL(*jni_, ImageProducerTextureEntryOnHardwareBufferReleased(
                         _, kBufferA, Ge(0)))
      .WillOnce(Invoke(CloseFence));
  EXPECT_CALL(*jni_, ImageProducerTextureEntryOnHardwareBufferReleased(
                         _, kBufferB, Ge(0)))
      .WillOnce(Invoke(CloseFence));

  RunFrame([&]() {
    texture->Sample(kBufferA);
    texture->Sample(kBufferB);
    texture->Sample(kBufferC);
  });
  FinishFrame();

  ASSERT_EQ(texture->requests->images.size(), 1u);
  EXPECT_EQ(texture->requests->images[0].size(), 2u);
}

TEST_F(ImageExternalTextureTest, ImageFrameReleasesTheSampledBuffer) {
  auto texture = MakeTexture();
  EXPECT_CALL(*jni_, ImageProducerTextureEntryOnHardwareBufferReleased(
                         _, kBufferA, Ge(0)))
      .WillOnce(Invoke(CloseFence));

  RunFrame([&]() {
    texture->Sample(kBufferA);
    texture->Sample(nullptr);
  });
  FinishFrame();

  // Nothing is sampled any more, so a later frame releases nothing.
  RunFrame([&]() { texture->Sample(nullptr); });
  FinishFrame();
  EXPECT_EQ(texture->requests->images.size(), 1u);
}

TEST_F(ImageExternalTextureTest, RepushedBufferReleasesItsEarlierPushOnly) {
  auto texture = MakeTexture();
  auto image = std::make_shared<int>(0);

  // Every push is released once, even when a producer pushes the buffer that
  // is on screen; the buffer stays in use, so its image is not handed over.
  EXPECT_CALL(*jni_, ImageProducerTextureEntryOnHardwareBufferReleased(
                         _, kBufferA, Ge(0)))
      .WillOnce(Invoke(CloseFence));

  RunFrame([&]() {
    texture->Sample(kBufferA, image);
    texture->Sample(kBufferA, image);
  });
  FinishFrame();

  ASSERT_EQ(texture->requests->images.size(), 1u);
  ASSERT_EQ(texture->requests->images[0].size(), 1u);
  EXPECT_EQ(texture->requests->images[0][0], nullptr);
}

TEST_F(ImageExternalTextureTest, ReplacedImageGoesToTheBackend) {
  auto texture = MakeTexture();
  auto image_a = std::make_shared<int>(0);
  EXPECT_CALL(*jni_, ImageProducerTextureEntryOnHardwareBufferReleased(
                         _, kBufferA, Ge(0)))
      .WillOnce(Invoke(CloseFence));

  RunFrame([&]() {
    texture->Sample(kBufferA, image_a);
    texture->Sample(kBufferB, std::make_shared<int>(0));
  });
  FinishFrame();

  ASSERT_EQ(texture->requests->images.size(), 1u);
  ASSERT_EQ(texture->requests->images[0].size(), 1u);
  EXPECT_EQ(texture->requests->images[0][0], image_a);
}

TEST_F(ImageExternalTextureTest, BufferWithoutListenerGetsNoFenceOrCallback) {
  auto texture = MakeTexture();
  EXPECT_CALL(*jni_, ImageProducerTextureEntryOnHardwareBufferReleased(_, _, _))
      .Times(0);

  RunFrame([&]() {
    texture->Sample(kBufferA, std::make_shared<int>(0),
                    /*wants_release=*/false);
    texture->Sample(kBufferB);
  });
  FinishFrame();

  // The backend still sees the image (to hand ownership back), but no fence
  // is made for it.
  ASSERT_EQ(texture->requests->want_fence.size(), 1u);
  EXPECT_FALSE(texture->requests->want_fence[0]);
  EXPECT_EQ(texture->requests->images[0].size(), 1u);
}

TEST_F(ImageExternalTextureTest, UnexportableFenceReleasesOnceDone) {
  auto texture = MakeTexture();
  texture->requests->exportable = false;
  EXPECT_CALL(*jni_, ImageProducerTextureEntryOnHardwareBufferReleased(_, _, _))
      .Times(0);

  RunFrame([&]() {
    texture->Sample(kBufferA);
    texture->Sample(kBufferB);
  });
  FinishFrame();
  // The work is not done, so A is not released, and nothing blocked.
  ::testing::Mock::VerifyAndClearExpectations(jni_.get());

  EXPECT_CALL(*jni_, ImageProducerTextureEntryOnHardwareBufferReleased(
                         _, kBufferA, -1));
  RunFrame([&]() { texture->requests->done = true; });
  // Let the next check come due.
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  FinishFrame();
}

TEST_F(ImageExternalTextureTest, UnsampledBufferIsReleasedAtOnce) {
  auto texture = MakeTexture();
  EXPECT_CALL(*jni_, ImageProducerTextureEntryOnHardwareBufferReleased(
                         _, kBufferA, -1));

  RunFrame([&]() { texture->DropUnsampled(kBufferA); });
}

TEST_F(ImageExternalTextureTest, ResetLifecycleReleasesOnContextDestroyed) {
  auto texture = MakeTexture();
  texture->MarkAttached();
  EXPECT_CALL(*jni_, ImageProducerTextureEntryOnHardwareBufferReleased(
                         _, kBufferA, Ge(0)))
      .WillOnce(Invoke(CloseFence));

  RunFrame([&]() {
    texture->Sample(kBufferA);
    static_cast<ContextListener&>(*texture).OnGrContextDestroyed();
  });
  FinishFrame();
}

TEST_F(ImageExternalTextureTest,
       KeepAliveLifecycleKeepsBufferOnContextDestroyed) {
  auto texture = MakeTexture(ImageExternalTexture::ImageLifecycle::kKeepAlive);
  texture->MarkAttached();

  // The last image is kept for when the context returns, so its buffer stays
  // with the texture.
  EXPECT_CALL(*jni_, ImageProducerTextureEntryOnHardwareBufferReleased(_, _, _))
      .Times(0);

  RunFrame([&]() {
    texture->Sample(kBufferA);
    static_cast<ContextListener&>(*texture).OnGrContextDestroyed();
  });
  FinishFrame();
}

}  // namespace testing
}  // namespace flutter
