// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/impeller/toolkit/android/surface_transaction.h"

#include <android/data_space.h>

#include "flutter/impeller/toolkit/android/hardware_buffer.h"
#include "flutter/impeller/toolkit/android/surface_control.h"
#include "impeller/base/validation.h"

namespace impeller::android {

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

  // Signal extended range brightness to the compositor (API 34+).
  // Without this, ADATASPACE_SCRGB values >1.0 are NOT displayed brighter —
  // the compositor defaults to desiredRatio=1.0 (SDR only).
  //
  // Declared range matches BT.2408's HLG peak-to-SDR-white ratio (10×).
  // The app-side shaders (video_ycbcr.frag and photo_edit.frag) pre-tone-map
  // their output to the actual display headroom via the BT.2390 EETF, so
  // buffer contents stay within what the display can represent regardless
  // of what we declare here. Declaring 10.0 ensures the compositor grants
  // whatever headroom the display physically supports (it will clamp down
  // to the panel's capability). This avoids relying on OEM compositor
  // tone-mapping behaviour, which varies across devices.
  const bool erb_available =
      GetProcTable()
          .ASurfaceTransaction_setExtendedRangeBrightness.IsAvailable();
  if (is_f16 && erb_available) {
    GetProcTable().ASurfaceTransaction_setExtendedRangeBrightness(
        transaction_.get().tx,  //
        control->GetHandle(),   //
        10.0f,                  // currentBufferRatio: BT.2408 HLG peak / SDR
        10.0f                   // desiredRatio: request full BT.2408 headroom
    );
  }

  if (!logged_once) {
    FML_LOG(INFO) << "SetContents: format="
                  << (is_f16 ? "F16" : "RGBA8")
                  << ", setBufferDataSpace available=" << ds_available
                  << ", dataspace=" << (is_f16 ? "SCRGB" : "default")
                  << ", setExtendedRangeBrightness available="
                  << erb_available
                  << ", extendedRange=10.0/10.0 (BT.2408 HLG peak)";
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
