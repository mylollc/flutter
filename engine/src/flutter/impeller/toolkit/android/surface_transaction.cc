// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/impeller/toolkit/android/surface_transaction.h"

#include <android/data_space.h>

#include <atomic>

#include "flutter/impeller/toolkit/android/hardware_buffer.h"
#include "flutter/impeller/toolkit/android/surface_control.h"
#include "impeller/base/validation.h"

namespace impeller::android {

namespace {
// App-declared HDR/SDR headroom for the onscreen extended-range surface,
// consumed by SetContents' ASurfaceTransaction_setExtendedRangeBrightness call.
// Defaults to 1.0 (SDR), so non-HDR apps are unaffected. The embedder sets it
// (FlutterRenderer.setHdrHeadroom -> FlutterJNI ->
// SetExtendedRangeBrightnessRatio) to the display headroom it is rendering HDR
// content against. Atomic: written on the platform thread, read on the raster
// thread at present time.
//
// Android exposes a single onscreen surface and the HDR/SDR headroom is a
// display-level property, so a module-level value (rather than per-surface
// threading through the raster-thread-owned swapchain) is sufficient.
std::atomic<float> g_extended_range_brightness_ratio{1.0f};
}  // namespace

void SetExtendedRangeBrightnessRatio(float ratio) {
  // HDR/SDR ratios are >= 1.0 by definition; clamp so a stray value never
  // signals an invalid (SDR-shrinking) range to the compositor.
  g_extended_range_brightness_ratio.store(ratio < 1.0f ? 1.0f : ratio,
                                          std::memory_order_relaxed);
}

float GetExtendedRangeBrightnessRatio() {
  return g_extended_range_brightness_ratio.load(std::memory_order_relaxed);
}

SurfaceTransaction::SurfaceTransaction()
    : transaction_(
          WrappedSurfaceTransaction{GetProcTable().ASurfaceTransaction_create(),
                                    /*owned=*/true}) {}

SurfaceTransaction::SurfaceTransaction(ASurfaceTransaction* transaction)
    : transaction_(WrappedSurfaceTransaction{transaction, /*owned=*/false}) {}

SurfaceTransaction::~SurfaceTransaction() = default;

bool SurfaceTransaction::IsValid() const {
  return transaction_.is_valid();
}

struct TransactionInFlightData {
  SurfaceTransaction::OnCompleteCallback callback;
};

bool SurfaceTransaction::Apply(OnCompleteCallback callback) {
  if (!IsValid()) {
    return false;
  }

  if (!callback) {
    callback = [](auto) {};
  }

  const auto& proc_table = GetProcTable();

  auto data = std::make_unique<TransactionInFlightData>();
  data->callback = callback;
  proc_table.ASurfaceTransaction_setOnComplete(
      transaction_.get().tx,  //
      data.release(),         //
      [](void* context, ASurfaceTransactionStats* stats) -> void {
        auto data = reinterpret_cast<TransactionInFlightData*>(context);
        data->callback(stats);
        delete data;
      });
  // If the transaction was created in Java, then it must be applied in
  // the Java PlatformViewController and not as a part of the engine render
  // loop.
  if (!transaction_.get().owned) {
    transaction_.reset();
    return true;
  }

  proc_table.ASurfaceTransaction_apply(transaction_.get().tx);

  // Transactions may not be applied over and over.
  transaction_.reset();
  return true;
}

bool SurfaceTransaction::SetContents(const SurfaceControl* control,
                                     const HardwareBuffer* buffer,
                                     fml::UniqueFD acquire_fence) {
  if (control == nullptr || buffer == nullptr) {
    VALIDATION_LOG << "Invalid control or buffer.";
    return false;
  }
  GetProcTable().ASurfaceTransaction_setBuffer(
      transaction_.get().tx,                                   //
      control->GetHandle(),                                    //
      buffer->GetHandle(),                                     //
      acquire_fence.is_valid() ? acquire_fence.release() : -1  //
  );

  // For F16 buffers, set ExtendedSRGB dataspace so the compositor preserves
  // values outside [0,1] (HDR/EDR headroom). This is the Android equivalent
  // of iOS/macOS kCGColorSpaceExtendedSRGB.
  static bool logged_once = false;
  const bool is_f16 = buffer->GetDescriptor().format ==
                       HardwareBufferFormat::kR16G16B16A16Float;
  const bool ds_available =
      GetProcTable().ASurfaceTransaction_setBufferDataSpace.IsAvailable();

  if (is_f16 && ds_available) {
    GetProcTable().ASurfaceTransaction_setBufferDataSpace(
        transaction_.get().tx,  //
        control->GetHandle(),   //
        ADATASPACE_SCRGB        // gamma-encoded ExtendedSRGB
    );
  }

  // Signal extended-range brightness to the compositor (API 34+). Without
  // this, ADATASPACE_SCRGB values >1.0 are NOT displayed brighter — the
  // compositor defaults to a 1.0 HDR/SDR ratio (SDR only).
  //
  // The ratio is the app-declared HDR/SDR headroom
  // (SetExtendedRangeBrightnessRatio), i.e. the display headroom the embedder
  // is rendering its HDR content against. The compositor maps the F16 buffer's
  // [1.0, ratio] range onto the panel's available headroom. Defaults to 1.0
  // (SDR) until the embedder declares one.
  const bool erb_available =
      GetProcTable()
          .ASurfaceTransaction_setExtendedRangeBrightness.IsAvailable();
  const float erb_ratio = GetExtendedRangeBrightnessRatio();
  if (is_f16 && erb_available) {
    GetProcTable().ASurfaceTransaction_setExtendedRangeBrightness(
        transaction_.get().tx,  //
        control->GetHandle(),   //
        erb_ratio,              // currentBufferRatio
        erb_ratio               // desiredRatio
    );
  }

  if (!logged_once) {
    FML_LOG(INFO) << "SetContents: format=" << (is_f16 ? "F16" : "RGBA8")
                  << ", setBufferDataSpace available=" << ds_available
                  << ", dataspace=" << (is_f16 ? "SCRGB" : "default")
                  << ", setExtendedRangeBrightness available=" << erb_available
                  << ", extendedRange=" << erb_ratio;
    logged_once = true;
  }

  return true;
}

bool SurfaceTransaction::SetBackgroundColor(const SurfaceControl& control,
                                            const Color& color) {
  if (!IsValid() || !control.IsValid()) {
    return false;
  }
  GetProcTable().ASurfaceTransaction_setColor(transaction_.get().tx,  //
                                              control.GetHandle(),    //
                                              color.red,              //
                                              color.green,            //
                                              color.blue,             //
                                              color.alpha,            //
                                              ADATASPACE_SRGB_LINEAR  //
  );
  return true;
}

bool SurfaceTransaction::SetParent(const SurfaceControl& control,
                                   const SurfaceControl* new_parent) {
  if (!IsValid() || !control.IsValid()) {
    return false;
  }
  if (new_parent && !new_parent->IsValid()) {
    return false;
  }
  GetProcTable().ASurfaceTransaction_reparent(
      transaction_.get().tx,                                     //
      control.GetHandle(),                                       //
      new_parent == nullptr ? nullptr : new_parent->GetHandle()  //
  );
  return true;
}

bool SurfaceTransaction::IsAvailableOnPlatform() {
  return GetProcTable().IsValid() &&
         GetProcTable().ASurfaceTransaction_create.IsAvailable();
}

}  // namespace impeller::android
