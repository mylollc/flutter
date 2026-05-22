// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/windows/direct_manipulation.h"

#include "flutter/fml/macros.h"
#include "flutter/shell/platform/windows/testing/mock_window_binding_handler_delegate.h"
#include "gtest/gtest.h"

using testing::_;

namespace flutter {
namespace testing {

class MockIDirectManipulationViewport : public IDirectManipulationViewport {
 public:
  MockIDirectManipulationViewport() {}

  MOCK_METHOD0_WITH_CALLTYPE(STDMETHODCALLTYPE, AddRef, ULONG());
  MOCK_METHOD0_WITH_CALLTYPE(STDMETHODCALLTYPE, Release, ULONG());
  MOCK_METHOD2_WITH_CALLTYPE(STDMETHODCALLTYPE,
                             QueryInterface,
                             HRESULT(REFIID, void**));
  MOCK_METHOD0_WITH_CALLTYPE(STDMETHODCALLTYPE, Abandon, HRESULT());
  MOCK_METHOD1_WITH_CALLTYPE(STDMETHODCALLTYPE,
                             ActivateConfiguration,
                             HRESULT(DIRECTMANIPULATION_CONFIGURATION));
  MOCK_METHOD1_WITH_CALLTYPE(STDMETHODCALLTYPE,
                             AddConfiguration,
                             HRESULT(DIRECTMANIPULATION_CONFIGURATION));
  MOCK_METHOD1_WITH_CALLTYPE(STDMETHODCALLTYPE,
                             AddContent,
                             HRESULT(IDirectManipulationContent*));
  MOCK_METHOD3_WITH_CALLTYPE(STDMETHODCALLTYPE,
                             AddEventHandler,
                             HRESULT(HWND,
                                     IDirectManipulationViewportEventHandler*,
                                     DWORD*));
  MOCK_METHOD0_WITH_CALLTYPE(STDMETHODCALLTYPE, Disable, HRESULT());
  MOCK_METHOD0_WITH_CALLTYPE(STDMETHODCALLTYPE, Enable, HRESULT());
  MOCK_METHOD2_WITH_CALLTYPE(STDMETHODCALLTYPE,
                             GetPrimaryContent,
                             HRESULT(REFIID, void**));
  MOCK_METHOD1_WITH_CALLTYPE(STDMETHODCALLTYPE,
                             GetStatus,
                             HRESULT(DIRECTMANIPULATION_STATUS*));
  MOCK_METHOD3_WITH_CALLTYPE(STDMETHODCALLTYPE,
                             GetTag,
                             HRESULT(REFIID, void**, UINT32*));
  MOCK_METHOD1_WITH_CALLTYPE(STDMETHODCALLTYPE,
                             GetViewportRect,
                             HRESULT(RECT*));
  MOCK_METHOD0_WITH_CALLTYPE(STDMETHODCALLTYPE, ReleaseAllContacts, HRESULT());
  MOCK_METHOD1_WITH_CALLTYPE(STDMETHODCALLTYPE,
                             ReleaseContact,
                             HRESULT(UINT32));
  MOCK_METHOD1_WITH_CALLTYPE(STDMETHODCALLTYPE,
                             RemoveConfiguration,
                             HRESULT(DIRECTMANIPULATION_CONFIGURATION));
  MOCK_METHOD1_WITH_CALLTYPE(STDMETHODCALLTYPE,
                             RemoveContent,
                             HRESULT(IDirectManipulationContent*));
  MOCK_METHOD1_WITH_CALLTYPE(STDMETHODCALLTYPE,
                             RemoveEventHandler,
                             HRESULT(DWORD));
  MOCK_METHOD1_WITH_CALLTYPE(STDMETHODCALLTYPE,
                             SetChaining,
                             HRESULT(DIRECTMANIPULATION_MOTION_TYPES));
  MOCK_METHOD1_WITH_CALLTYPE(STDMETHODCALLTYPE, SetContact, HRESULT(UINT32));
  MOCK_METHOD1_WITH_CALLTYPE(STDMETHODCALLTYPE,
                             SetInputMode,
                             HRESULT(DIRECTMANIPULATION_INPUT_MODE));
  MOCK_METHOD1_WITH_CALLTYPE(STDMETHODCALLTYPE,
                             SetManualGesture,
                             HRESULT(DIRECTMANIPULATION_GESTURE_CONFIGURATION));
  MOCK_METHOD2_WITH_CALLTYPE(STDMETHODCALLTYPE,
                             SetTag,
                             HRESULT(IUnknown*, UINT32));
  MOCK_METHOD1_WITH_CALLTYPE(STDMETHODCALLTYPE,
                             SetUpdateMode,
                             HRESULT(DIRECTMANIPULATION_INPUT_MODE));
  MOCK_METHOD1_WITH_CALLTYPE(STDMETHODCALLTYPE,
                             SetViewportOptions,
                             HRESULT(DIRECTMANIPULATION_VIEWPORT_OPTIONS));
  MOCK_METHOD1_WITH_CALLTYPE(STDMETHODCALLTYPE,
                             SetViewportRect,
                             HRESULT(const RECT*));
  MOCK_METHOD2_WITH_CALLTYPE(STDMETHODCALLTYPE,
                             SetViewportTransform,
                             HRESULT(const float*, DWORD));
  MOCK_METHOD0_WITH_CALLTYPE(STDMETHODCALLTYPE, Stop, HRESULT());
  MOCK_METHOD2_WITH_CALLTYPE(STDMETHODCALLTYPE,
                             SyncDisplayTransform,
                             HRESULT(const float*, DWORD));
  MOCK_METHOD5_WITH_CALLTYPE(
      STDMETHODCALLTYPE,
      ZoomToRect,
      HRESULT(const float, const float, const float, const float, BOOL));

 private:
  FML_DISALLOW_COPY_AND_ASSIGN(MockIDirectManipulationViewport);
};

class MockIDirectManipulationContent : public IDirectManipulationContent {
 public:
  MockIDirectManipulationContent() {}

  MOCK_METHOD0_WITH_CALLTYPE(STDMETHODCALLTYPE, AddRef, ULONG());
  MOCK_METHOD0_WITH_CALLTYPE(STDMETHODCALLTYPE, Release, ULONG());
  MOCK_METHOD2_WITH_CALLTYPE(STDMETHODCALLTYPE,
                             QueryInterface,
                             HRESULT(REFIID, void**));
  MOCK_METHOD1_WITH_CALLTYPE(STDMETHODCALLTYPE, GetContentRect, HRESULT(RECT*));
  MOCK_METHOD2_WITH_CALLTYPE(STDMETHODCALLTYPE,
                             GetContentTransform,
                             HRESULT(float*, DWORD));
  MOCK_METHOD2_WITH_CALLTYPE(STDMETHODCALLTYPE,
                             GetOutputTransform,
                             HRESULT(float*, DWORD));
  MOCK_METHOD3_WITH_CALLTYPE(STDMETHODCALLTYPE,
                             GetTag,
                             HRESULT(REFIID, void**, UINT32*));
  MOCK_METHOD2_WITH_CALLTYPE(STDMETHODCALLTYPE,
                             GetViewport,
                             HRESULT(REFIID, void**));
  MOCK_METHOD1_WITH_CALLTYPE(STDMETHODCALLTYPE,
                             SetContentRect,
                             HRESULT(const RECT*));
  MOCK_METHOD2_WITH_CALLTYPE(STDMETHODCALLTYPE,
                             SetTag,
                             HRESULT(IUnknown*, UINT32));
  MOCK_METHOD2_WITH_CALLTYPE(STDMETHODCALLTYPE,
                             SyncContentTransform,
                             HRESULT(const float*, DWORD));

 private:
  FML_DISALLOW_COPY_AND_ASSIGN(MockIDirectManipulationContent);
};

TEST(DirectManipulationTest, TestGesture) {
  MockIDirectManipulationContent content;
  MockWindowBindingHandlerDelegate delegate;
  MockIDirectManipulationViewport viewport;
  const float scale = 1.5;
  const float pan_x = 32.0;
  const float pan_y = 16.0;
  const int DISPLAY_WIDTH = 800;
  const int DISPLAY_HEIGHT = 600;
  auto owner = std::make_unique<DirectManipulationOwner>(nullptr);
  owner->SetBindingHandlerDelegate(&delegate);
  auto handler =
      fml::MakeRefCounted<DirectManipulationEventHandler>(owner.get());
  int32_t device_id = (int32_t)reinterpret_cast<int64_t>(handler.get());
  EXPECT_CALL(viewport, GetPrimaryContent(_, _))
      .WillOnce(::testing::Invoke([&content](REFIID in, void** out) {
        *out = &content;
        return S_OK;
      }))
      .RetiresOnSaturation();
  EXPECT_CALL(content, GetContentTransform(_, 6))
      .WillOnce(::testing::Invoke([scale](float* transform, DWORD size) {
        transform[0] = 1.0f;
        transform[4] = 0.0;
        transform[5] = 0.0;
        return S_OK;
      }))
      .RetiresOnSaturation();
  EXPECT_CALL(delegate, OnPointerPanZoomStart(device_id));
  handler->OnViewportStatusChanged((IDirectManipulationViewport*)&viewport,
                                   DIRECTMANIPULATION_RUNNING,
                                   DIRECTMANIPULATION_READY);
  EXPECT_CALL(content, GetContentTransform(_, 6))
      .WillOnce(::testing::Invoke(
          [scale, pan_x, pan_y](float* transform, DWORD size) {
            transform[0] = scale;
            transform[4] = pan_x;
            transform[5] = pan_y;
            return S_OK;
          }));
  EXPECT_CALL(delegate,
              OnPointerPanZoomUpdate(device_id, pan_x, pan_y, scale, 0));
  handler->OnContentUpdated((IDirectManipulationViewport*)&viewport,
                            (IDirectManipulationContent*)&content);
  EXPECT_CALL(delegate, OnPointerPanZoomEnd(device_id));
  EXPECT_CALL(viewport, GetViewportRect(_))
      .WillOnce(::testing::Invoke([DISPLAY_WIDTH, DISPLAY_HEIGHT](RECT* rect) {
        rect->left = 0;
        rect->top = 0;
        rect->right = DISPLAY_WIDTH;
        rect->bottom = DISPLAY_HEIGHT;
        return S_OK;
      }));
  EXPECT_CALL(viewport, ZoomToRect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, false))
      .WillOnce(::testing::Return(S_OK));
  handler->OnViewportStatusChanged((IDirectManipulationViewport*)&viewport,
                                   DIRECTMANIPULATION_INERTIA,
                                   DIRECTMANIPULATION_RUNNING);
  handler->OnViewportStatusChanged((IDirectManipulationViewport*)&viewport,
                                   DIRECTMANIPULATION_READY,
                                   DIRECTMANIPULATION_INERTIA);
}

// Verify that scale mantissa rounding works as expected
TEST(DirectManipulationTest, TestRounding) {
  MockIDirectManipulationContent content;
  MockWindowBindingHandlerDelegate delegate;
  MockIDirectManipulationViewport viewport;
  const float scale = 1.5;
  const int DISPLAY_WIDTH = 800;
  const int DISPLAY_HEIGHT = 600;
  auto owner = std::make_unique<DirectManipulationOwner>(nullptr);
  owner->SetBindingHandlerDelegate(&delegate);
  auto handler =
      fml::MakeRefCounted<DirectManipulationEventHandler>(owner.get());
  int32_t device_id = (int32_t)reinterpret_cast<int64_t>(handler.get());
  EXPECT_CALL(viewport, GetPrimaryContent(_, _))
      .WillOnce(::testing::Invoke([&content](REFIID in, void** out) {
        *out = &content;
        return S_OK;
      }))
      .RetiresOnSaturation();
  EXPECT_CALL(content, GetContentTransform(_, 6))
      .WillOnce(::testing::Invoke([scale](float* transform, DWORD size) {
        transform[0] = 1.0f;
        transform[4] = 0.0;
        transform[5] = 0.0;
        return S_OK;
      }))
      .RetiresOnSaturation();
  EXPECT_CALL(delegate, OnPointerPanZoomStart(device_id));
  handler->OnViewportStatusChanged((IDirectManipulationViewport*)&viewport,
                                   DIRECTMANIPULATION_RUNNING,
                                   DIRECTMANIPULATION_READY);
  EXPECT_CALL(content, GetContentTransform(_, 6))
      .WillOnce(::testing::Invoke([scale](float* transform, DWORD size) {
        transform[0] = 1.5000001f;
        transform[4] = 4.0;
        transform[5] = 0.0;
        return S_OK;
      }))
      .RetiresOnSaturation();
  EXPECT_CALL(delegate,
              OnPointerPanZoomUpdate(device_id, 4.0, 0, 1.5000001f, 0))
      .Times(0);
  EXPECT_CALL(delegate, OnPointerPanZoomUpdate(device_id, 4.0, 0, 1.5f, 0))
      .Times(1)
      .RetiresOnSaturation();
  EXPECT_CALL(content, GetContentTransform(_, 6))
      .WillOnce(::testing::Invoke([scale](float* transform, DWORD size) {
        transform[0] = 1.50000065f;
        transform[4] = 2.0;
        transform[5] = 0.0;
        return S_OK;
      }))
      .RetiresOnSaturation();
  EXPECT_CALL(delegate,
              OnPointerPanZoomUpdate(device_id, 2.0, 0, 1.50000065f, 0))
      .Times(0);
  EXPECT_CALL(delegate,
              OnPointerPanZoomUpdate(device_id, 2.0, 0, 1.50000047f, 0))
      .Times(1)
      .RetiresOnSaturation();
  EXPECT_CALL(delegate, OnPointerPanZoomEnd(device_id));
  EXPECT_CALL(viewport, GetViewportRect(_))
      .WillOnce(::testing::Invoke([DISPLAY_WIDTH, DISPLAY_HEIGHT](RECT* rect) {
        rect->left = 0;
        rect->top = 0;
        rect->right = DISPLAY_WIDTH;
        rect->bottom = DISPLAY_HEIGHT;
        return S_OK;
      }));
  EXPECT_CALL(viewport, ZoomToRect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, false))
      .WillOnce(::testing::Return(S_OK));
  handler->OnContentUpdated((IDirectManipulationViewport*)&viewport,
                            (IDirectManipulationContent*)&content);
  handler->OnContentUpdated((IDirectManipulationViewport*)&viewport,
                            (IDirectManipulationContent*)&content);
  handler->OnViewportStatusChanged((IDirectManipulationViewport*)&viewport,
                                   DIRECTMANIPULATION_INERTIA,
                                   DIRECTMANIPULATION_RUNNING);
  handler->OnViewportStatusChanged((IDirectManipulationViewport*)&viewport,
                                   DIRECTMANIPULATION_READY,
                                   DIRECTMANIPULATION_INERTIA);
}

// Have some initial values in the matrix, only the differences should be
// reported.
TEST(DirectManipulationTest, TestGestureWithInitialData) {
  MockIDirectManipulationContent content;
  MockWindowBindingHandlerDelegate delegate;
  MockIDirectManipulationViewport viewport;
  const float scale = 1.5;
  const float pan_x = 32.0;
  const float pan_y = 16.0;
  const int DISPLAY_WIDTH = 800;
  const int DISPLAY_HEIGHT = 600;
  auto owner = std::make_unique<DirectManipulationOwner>(nullptr);
  owner->SetBindingHandlerDelegate(&delegate);
  auto handler =
      fml::MakeRefCounted<DirectManipulationEventHandler>(owner.get());
  int32_t device_id = (int32_t)reinterpret_cast<int64_t>(handler.get());
  EXPECT_CALL(viewport, GetPrimaryContent(_, _))
      .WillOnce(::testing::Invoke([&content](REFIID in, void** out) {
        *out = &content;
        return S_OK;
      }))
      .RetiresOnSaturation();
  EXPECT_CALL(content, GetContentTransform(_, 6))
      .WillOnce(::testing::Invoke([scale](float* transform, DWORD size) {
        transform[0] = 2.0f;
        transform[4] = 234.0;
        transform[5] = 345.0;
        return S_OK;
      }))
      .RetiresOnSaturation();
  EXPECT_CALL(delegate, OnPointerPanZoomStart(device_id));
  handler->OnViewportStatusChanged((IDirectManipulationViewport*)&viewport,
                                   DIRECTMANIPULATION_RUNNING,
                                   DIRECTMANIPULATION_READY);
  EXPECT_CALL(content, GetContentTransform(_, 6))
      .WillOnce(::testing::Invoke(
          [scale, pan_x, pan_y](float* transform, DWORD size) {
            transform[0] = 2.0f * scale;
            transform[4] = 234.0 + pan_x;
            transform[5] = 345.0 + pan_y;
            return S_OK;
          }));
  EXPECT_CALL(delegate,
              OnPointerPanZoomUpdate(device_id, pan_x, pan_y, scale, 0));
  handler->OnContentUpdated((IDirectManipulationViewport*)&viewport,
                            (IDirectManipulationContent*)&content);
  EXPECT_CALL(delegate, OnPointerPanZoomEnd(device_id));
  EXPECT_CALL(viewport, GetViewportRect(_))
      .WillOnce(::testing::Invoke([DISPLAY_WIDTH, DISPLAY_HEIGHT](RECT* rect) {
        rect->left = 0;
        rect->top = 0;
        rect->right = DISPLAY_WIDTH;
        rect->bottom = DISPLAY_HEIGHT;
        return S_OK;
      }));
  EXPECT_CALL(viewport, ZoomToRect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, false))
      .WillOnce(::testing::Return(S_OK));
  handler->OnViewportStatusChanged((IDirectManipulationViewport*)&viewport,
                                   DIRECTMANIPULATION_INERTIA,
                                   DIRECTMANIPULATION_RUNNING);
  handler->OnViewportStatusChanged((IDirectManipulationViewport*)&viewport,
                                   DIRECTMANIPULATION_READY,
                                   DIRECTMANIPULATION_INERTIA);
}

// OnContentUpdated calls that arrive while DM is in the INERTIA state
// (between RUNNING->INERTIA and INERTIA->READY) must be forwarded to the
// framework as OnPointerPanZoomUpdate events so the OS-driven deceleration
// stream reaches the framework's Scrollable. See flutter/flutter#126649.
TEST(DirectManipulationTest, TestInertiaFramesForwardedToFramework) {
  MockIDirectManipulationContent content;
  MockWindowBindingHandlerDelegate delegate;
  MockIDirectManipulationViewport viewport;
  const int DISPLAY_WIDTH = 800;
  const int DISPLAY_HEIGHT = 600;
  auto owner = std::make_unique<DirectManipulationOwner>(nullptr);
  owner->SetBindingHandlerDelegate(&delegate);
  auto handler =
      fml::MakeRefCounted<DirectManipulationEventHandler>(owner.get());
  int32_t device_id = (int32_t)reinterpret_cast<int64_t>(handler.get());

  // READY -> RUNNING: gesture starts. Initial transform at identity so
  // subsequent pan_y values are reported verbatim to the framework.
  EXPECT_CALL(viewport, GetPrimaryContent(_, _))
      .WillOnce(::testing::Invoke([&content](REFIID in, void** out) {
        *out = &content;
        return S_OK;
      }))
      .RetiresOnSaturation();
  EXPECT_CALL(content, GetContentTransform(_, 6))
      .WillOnce(::testing::Invoke([](float* transform, DWORD size) {
        transform[0] = 1.0f;
        transform[4] = 0.0;
        transform[5] = 0.0;
        return S_OK;
      }))
      .RetiresOnSaturation();
  EXPECT_CALL(delegate, OnPointerPanZoomStart(device_id));
  handler->OnViewportStatusChanged((IDirectManipulationViewport*)&viewport,
                                   DIRECTMANIPULATION_RUNNING,
                                   DIRECTMANIPULATION_READY);

  // RUNNING phase: one user-motion update at pan_y = 50.
  EXPECT_CALL(content, GetContentTransform(_, 6))
      .WillOnce(::testing::Invoke([](float* transform, DWORD size) {
        transform[0] = 1.0f;
        transform[4] = 0.0;
        transform[5] = 50.0;
        return S_OK;
      }))
      .RetiresOnSaturation();
  EXPECT_CALL(delegate, OnPointerPanZoomUpdate(device_id, 0.0, 50.0, 1.0f, 0))
      .RetiresOnSaturation();
  handler->OnContentUpdated((IDirectManipulationViewport*)&viewport,
                            (IDirectManipulationContent*)&content);

  // RUNNING -> INERTIA: gesture stays alive; no PanZoomEnd here.
  handler->OnViewportStatusChanged((IDirectManipulationViewport*)&viewport,
                                   DIRECTMANIPULATION_INERTIA,
                                   DIRECTMANIPULATION_RUNNING);

  // Two OS-driven inertia frames must each produce an OnPointerPanZoomUpdate.
  // Pan values decay (50 -> 70 -> 75) so deltas shrink each frame, mimicking
  // natural OS inertia decay.
  EXPECT_CALL(content, GetContentTransform(_, 6))
      .WillOnce(::testing::Invoke([](float* transform, DWORD size) {
        transform[0] = 1.0f;
        transform[4] = 0.0;
        transform[5] = 70.0;
        return S_OK;
      }))
      .RetiresOnSaturation();
  EXPECT_CALL(delegate, OnPointerPanZoomUpdate(device_id, 0.0, 70.0, 1.0f, 0))
      .RetiresOnSaturation();
  handler->OnContentUpdated((IDirectManipulationViewport*)&viewport,
                            (IDirectManipulationContent*)&content);

  EXPECT_CALL(content, GetContentTransform(_, 6))
      .WillOnce(::testing::Invoke([](float* transform, DWORD size) {
        transform[0] = 1.0f;
        transform[4] = 0.0;
        transform[5] = 75.0;
        return S_OK;
      }))
      .RetiresOnSaturation();
  EXPECT_CALL(delegate, OnPointerPanZoomUpdate(device_id, 0.0, 75.0, 1.0f, 0))
      .RetiresOnSaturation();
  handler->OnContentUpdated((IDirectManipulationViewport*)&viewport,
                            (IDirectManipulationContent*)&content);

  // Final inertia frame with no motion (delta 0) -- simulates the last
  // settled frame before INERTIA->READY.
  EXPECT_CALL(content, GetContentTransform(_, 6))
      .WillOnce(::testing::Invoke([](float* transform, DWORD size) {
        transform[0] = 1.0f;
        transform[4] = 0.0;
        transform[5] = 75.0;
        return S_OK;
      }))
      .RetiresOnSaturation();
  EXPECT_CALL(delegate, OnPointerPanZoomUpdate(device_id, 0.0, 75.0, 1.0f, 0))
      .RetiresOnSaturation();
  handler->OnContentUpdated((IDirectManipulationViewport*)&viewport,
                            (IDirectManipulationContent*)&content);

  // INERTIA -> READY: gesture truly ends now; PanZoomEnd fires once here.
  EXPECT_CALL(delegate, OnPointerPanZoomEnd(device_id));
  EXPECT_CALL(viewport, GetViewportRect(_))
      .WillOnce(::testing::Invoke([DISPLAY_WIDTH, DISPLAY_HEIGHT](RECT* rect) {
        rect->left = 0;
        rect->top = 0;
        rect->right = DISPLAY_WIDTH;
        rect->bottom = DISPLAY_HEIGHT;
        return S_OK;
      }));
  EXPECT_CALL(viewport, ZoomToRect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, false))
      .WillOnce(::testing::Return(S_OK));
  handler->OnViewportStatusChanged((IDirectManipulationViewport*)&viewport,
                                   DIRECTMANIPULATION_READY,
                                   DIRECTMANIPULATION_INERTIA);
}

// When the user re-touches the trackpad while OS inertia is still animating
// (INERTIA -> RUNNING transition), the prior gesture must be ended cleanly
// with OnPointerPanZoomEnd and a fresh OnPointerPanZoomStart fired for the
// new gesture. This guarantees every PanZoomStart has a matching PanZoomEnd
// so the framework's gesture recognizers stay in a consistent state across
// rapid repeated swipes.
TEST(DirectManipulationTest, TestInertiaInterruptedByNewGesture) {
  MockIDirectManipulationContent content;
  MockWindowBindingHandlerDelegate delegate;
  MockIDirectManipulationViewport viewport;
  auto owner = std::make_unique<DirectManipulationOwner>(nullptr);
  owner->SetBindingHandlerDelegate(&delegate);
  auto handler =
      fml::MakeRefCounted<DirectManipulationEventHandler>(owner.get());
  int32_t device_id = (int32_t)reinterpret_cast<int64_t>(handler.get());

  // First gesture: READY -> RUNNING fires PanZoomStart (gesture #1).
  EXPECT_CALL(viewport, GetPrimaryContent(_, _))
      .WillOnce(::testing::Invoke([&content](REFIID in, void** out) {
        *out = &content;
        return S_OK;
      }))
      .RetiresOnSaturation();
  EXPECT_CALL(content, GetContentTransform(_, 6))
      .WillOnce(::testing::Invoke([](float* transform, DWORD size) {
        transform[0] = 1.0f;
        transform[4] = 0.0;
        transform[5] = 0.0;
        return S_OK;
      }))
      .RetiresOnSaturation();
  EXPECT_CALL(delegate, OnPointerPanZoomStart(device_id)).RetiresOnSaturation();
  handler->OnViewportStatusChanged((IDirectManipulationViewport*)&viewport,
                                   DIRECTMANIPULATION_RUNNING,
                                   DIRECTMANIPULATION_READY);

  // RUNNING -> INERTIA: no PanZoomEnd; gesture #1 still alive.
  handler->OnViewportStatusChanged((IDirectManipulationViewport*)&viewport,
                                   DIRECTMANIPULATION_INERTIA,
                                   DIRECTMANIPULATION_RUNNING);

  // INERTIA -> RUNNING (user re-touched the trackpad): gesture #1 ends and
  // gesture #2 begins -- both events fire on this single status change.
  EXPECT_CALL(delegate, OnPointerPanZoomEnd(device_id)).RetiresOnSaturation();
  EXPECT_CALL(viewport, GetPrimaryContent(_, _))
      .WillOnce(::testing::Invoke([&content](REFIID in, void** out) {
        *out = &content;
        return S_OK;
      }))
      .RetiresOnSaturation();
  EXPECT_CALL(content, GetContentTransform(_, 6))
      .WillOnce(::testing::Invoke([](float* transform, DWORD size) {
        transform[0] = 1.0f;
        transform[4] = 0.0;
        transform[5] = 0.0;
        return S_OK;
      }))
      .RetiresOnSaturation();
  EXPECT_CALL(delegate, OnPointerPanZoomStart(device_id)).RetiresOnSaturation();
  handler->OnViewportStatusChanged((IDirectManipulationViewport*)&viewport,
                                   DIRECTMANIPULATION_RUNNING,
                                   DIRECTMANIPULATION_INERTIA);
}

}  // namespace testing
}  // namespace flutter
