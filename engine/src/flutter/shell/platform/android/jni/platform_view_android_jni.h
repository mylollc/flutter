// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_ANDROID_JNI_PLATFORM_VIEW_ANDROID_JNI_H_
#define FLUTTER_SHELL_PLATFORM_ANDROID_JNI_PLATFORM_VIEW_ANDROID_JNI_H_

#include <utility>

#include "flutter/fml/mapping.h"

#include "flutter/flow/embedded_views.h"
#include "flutter/lib/ui/window/platform_message.h"
#include "flutter/shell/platform/android/surface/android_native_window.h"

#if FML_OS_ANDROID
#include <android/hardware_buffer.h>

#include "flutter/fml/platform/android/scoped_java_ref.h"
#endif

struct ASurfaceTransaction;

namespace flutter {

#if FML_OS_ANDROID
using JavaLocalRef = fml::jni::ScopedJavaLocalRef<jobject>;
#else
using JavaLocalRef = std::nullptr_t;
// Forward-declare the NDK opaque type on non-Android hosts so the
// interface signatures below compile for host-side unit-test builds.
// Members stay compile-only; nothing constructs an AHardwareBuffer off
// Android.
using AHardwareBuffer = struct AHardwareBuffer;
#endif

/// Plain-data handle for a raw `AHardwareBuffer` + its acquire fence +
/// release-ack fd, returned by
/// [PlatformViewAndroidJNI::ImageProducerTextureEntryAcquireLatestHardwareBuffer].
///
/// Ownership contract: the engine transfers one AHB reference to the
/// consumer on acquisition. The consumer must eventually release it via
/// `AHardwareBuffer_release`. Both fds (when not -1) are transferred too:
///
/// - `acquire_fence_fd`: consumer waits on it (or imports into a Vulkan
///   semaphore) before sampling, then closes.
/// - `release_ack_fd`: consumer signals it (`write(fd, &u64, 8)`, matching
///   the producer's `eventfd`) when GPU sampling completes, then closes.
///   This unblocks the producer so it can safely reuse the AHB. When
///   the consumer never reaches the sampling path (early-return / error),
///   it must still signal+close the ack fd — otherwise the producer
///   polls forever.
struct AcquiredHardwareBuffer {
  AHardwareBuffer* buffer = nullptr;
  int acquire_fence_fd = -1;
  int release_ack_fd = -1;
};

//------------------------------------------------------------------------------
/// Allows to call Java code running in the JVM from any thread. However, most
/// methods can only be called from the platform thread as that is where the
/// Java code runs.
///
/// This interface must not depend on the Android toolchain directly, so it can
/// be used in unit tests compiled with the host toolchain.
///
class PlatformViewAndroidJNI {
 public:
  virtual ~PlatformViewAndroidJNI();

  //----------------------------------------------------------------------------
  /// @brief      Sends a platform message. The message may be empty.
  ///
  virtual void FlutterViewHandlePlatformMessage(
      std::unique_ptr<flutter::PlatformMessage> message,
      int responseId) = 0;

  //----------------------------------------------------------------------------
  /// @brief      Responds to a platform message. The data may be a `nullptr`.
  ///
  virtual void FlutterViewHandlePlatformMessageResponse(
      int responseId,
      std::unique_ptr<fml::Mapping> data) = 0;

  //----------------------------------------------------------------------------
  /// @brief      Sends semantics tree updates.
  ///
  /// @note       Must be called from the platform thread.
  ///
  virtual void FlutterViewUpdateSemantics(
      std::vector<uint8_t> buffer,
      std::vector<std::string> strings,
      std::vector<std::vector<uint8_t>> string_attribute_args) = 0;

  //----------------------------------------------------------------------------
  /// @brief      Set application locale to a given language.
  ///
  /// @note       Must be called from the platform thread.
  ///
  virtual void FlutterViewSetApplicationLocale(std::string locale) = 0;

  //----------------------------------------------------------------------------
  /// @brief      Enables or disables the semantics tree.
  ///
  /// @note       Must be called from the platform thread.
  ///
  virtual void FlutterViewSetSemanticsTreeEnabled(bool enabled) = 0;

  //----------------------------------------------------------------------------
  /// @brief      Sends new custom accessibility events.
  ///
  /// @note       Must be called from the platform thread.
  ///
  virtual void FlutterViewUpdateCustomAccessibilityActions(
      std::vector<uint8_t> actions_buffer,
      std::vector<std::string> strings) = 0;

  //----------------------------------------------------------------------------
  /// @brief      Indicates that FlutterView should start painting pixels.
  ///
  /// @note       Must be called from the platform thread.
  ///
  virtual void FlutterViewOnFirstFrame() = 0;

  //----------------------------------------------------------------------------
  /// @brief      Indicates that a hot restart is about to happen.
  ///
  virtual void FlutterViewOnPreEngineRestart() = 0;

  //----------------------------------------------------------------------------
  /// @brief      Attach the SurfaceTexture to the OpenGL ES context that is
  ///             current on the calling thread.
  ///
  virtual void SurfaceTextureAttachToGLContext(JavaLocalRef surface_texture,
                                               int textureId) = 0;

  //----------------------------------------------------------------------------
  /// @brief      Returns true if surface_texture should be updated.
  ///
  virtual bool SurfaceTextureShouldUpdate(JavaLocalRef surface_texture) = 0;

  //----------------------------------------------------------------------------
  /// @brief      Updates the texture image to the most recent frame from the
  ///             image stream.
  ///
  virtual void SurfaceTextureUpdateTexImage(JavaLocalRef surface_texture) = 0;

  //----------------------------------------------------------------------------
  /// @brief      Gets the transform matrix from the SurfaceTexture.
  ///             Then, it updates the `transform` matrix, so it fill the canvas
  ///             and preserve the aspect ratio.
  ///
  virtual SkM44 SurfaceTextureGetTransformMatrix(
      JavaLocalRef surface_texture) = 0;

  //----------------------------------------------------------------------------
  /// @brief      Detaches a SurfaceTexture from the OpenGL ES context.
  ///
  virtual void SurfaceTextureDetachFromGLContext(
      JavaLocalRef surface_texture) = 0;

  //----------------------------------------------------------------------------
  /// @brief      Acquire the latest image available.
  ///
  virtual JavaLocalRef ImageProducerTextureEntryAcquireLatestImage(
      JavaLocalRef image_texture_entry) = 0;

  //----------------------------------------------------------------------------
  /// @brief      Acquire the latest raw `AHardwareBuffer` pushed via the
  ///             direct-AHB path on `ImageTextureEntry`.
  ///
  ///             Returns an `AcquiredHardwareBuffer` with `buffer == nullptr`
  ///             when no AHB is pending — the entry is either untouched, or
  ///             operating on the legacy `Image`-based flow
  ///             (`acquireLatestImage`). The returned AHB (when non-null) and
  ///             fence fd (when not -1) are owned by the caller per the
  ///             `AcquiredHardwareBuffer` contract.
  ///
  ///             Default implementation returns an empty handle so
  ///             pre-direct-AHB test fakes and host-side stubs keep compiling.
  ///
  virtual AcquiredHardwareBuffer
  ImageProducerTextureEntryAcquireLatestHardwareBuffer(
      JavaLocalRef image_texture_entry) {
    return AcquiredHardwareBuffer{};
  }

  //----------------------------------------------------------------------------
  /// @brief      Grab the HardwareBuffer from image.
  ///
  virtual JavaLocalRef ImageGetHardwareBuffer(JavaLocalRef image) = 0;

  //----------------------------------------------------------------------------
  /// @brief      Call close on image.
  ///
  virtual void ImageClose(JavaLocalRef image) = 0;

  //----------------------------------------------------------------------------
  /// @brief      Call close on hardware_buffer.
  ///
  virtual void HardwareBufferClose(JavaLocalRef hardware_buffer) = 0;

  //----------------------------------------------------------------------------
  /// @brief      Positions and sizes a platform view if using hybrid
  ///             composition.
  ///
  /// @note       Must be called from the platform thread.
  ///
  virtual void FlutterViewOnDisplayPlatformView(
      int view_id,
      int x,
      int y,
      int width,
      int height,
      int viewWidth,
      int viewHeight,
      MutatorsStack mutators_stack) = 0;

  //----------------------------------------------------------------------------
  /// @brief      Positions and sizes an overlay surface in hybrid composition.
  ///
  /// @note       Must be called from the platform thread.
  ///
  virtual void FlutterViewDisplayOverlaySurface(int surface_id,
                                                int x,
                                                int y,
                                                int width,
                                                int height) = 0;

  //----------------------------------------------------------------------------
  /// @brief      Initiates a frame if using hybrid composition.
  ///
  ///
  /// @note       Must be called from the platform thread.
  ///
  virtual void FlutterViewBeginFrame() = 0;

  //----------------------------------------------------------------------------
  /// @brief      Indicates that the current frame ended.
  ///             It's used to clean up state.
  ///
  /// @note       Must be called from the platform thread.
  ///
  virtual void FlutterViewEndFrame() = 0;

  //------------------------------------------------------------------------------
  /// The metadata returned from Java which is converted into an |OverlayLayer|
  /// by |SurfacePool|.
  ///
  struct OverlayMetadata {
    OverlayMetadata(int id, fml::RefPtr<AndroidNativeWindow> window)
        : id(id), window(std::move(window)) {};

    ~OverlayMetadata() = default;

    // A unique id to identify the overlay when it gets recycled.
    const int id;

    // Holds a reference to the native window. That is, an `ANativeWindow`,
    // which is the C counterpart of the `android.view.Surface` object in Java.
    const fml::RefPtr<AndroidNativeWindow> window;
  };

  //----------------------------------------------------------------------------
  /// @brief      Instantiates an overlay surface in hybrid composition and
  ///             provides the necessary metadata to operate the surface in C.
  ///
  /// @note       Must be called from the platform thread.
  ///
  virtual std::unique_ptr<PlatformViewAndroidJNI::OverlayMetadata>
  FlutterViewCreateOverlaySurface() = 0;

  //----------------------------------------------------------------------------
  /// @brief      Destroys the overlay surfaces.
  ///
  /// @note       Must be called from the platform thread.
  ///
  virtual void FlutterViewDestroyOverlaySurfaces() = 0;

  // New Platform View Support.
  virtual ASurfaceTransaction* createTransaction() = 0;

  virtual void swapTransaction() = 0;

  virtual void applyTransaction() = 0;

  virtual std::unique_ptr<PlatformViewAndroidJNI::OverlayMetadata>
  createOverlaySurface2() = 0;

  virtual void destroyOverlaySurface2() = 0;

  virtual void onEndFrame2() = 0;

  virtual void onDisplayPlatformView2(int32_t view_id,
                                      int32_t x,
                                      int32_t y,
                                      int32_t width,
                                      int32_t height,
                                      int32_t viewWidth,
                                      int32_t viewHeight,
                                      MutatorsStack mutators_stack) = 0;

  virtual void hidePlatformView2(int32_t view_id) = 0;

  virtual void showOverlaySurface2() = 0;

  virtual void hideOverlaySurface2() = 0;

  //----------------------------------------------------------------------------
  /// @brief      Computes the locale Android would select.
  ///
  virtual std::unique_ptr<std::vector<std::string>>
  FlutterViewComputePlatformResolvedLocale(
      std::vector<std::string> supported_locales_data) = 0;

  virtual double GetDisplayRefreshRate() = 0;

  virtual double GetDisplayWidth() = 0;

  virtual double GetDisplayHeight() = 0;

  virtual double GetDisplayDensity() = 0;

  virtual bool RequestDartDeferredLibrary(int loading_unit_id) = 0;

  virtual double FlutterViewGetScaledFontSize(double unscaled_font_size,
                                              int configuration_id) const = 0;

  virtual void MaybeResizeSurfaceView(int32_t width, int32_t height) const = 0;
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_ANDROID_JNI_PLATFORM_VIEW_ANDROID_JNI_H_
