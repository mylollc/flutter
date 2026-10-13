// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package io.flutter.view;

import static io.flutter.Build.API_LEVELS;

import android.graphics.SurfaceTexture;
import android.media.Image;
import android.os.ParcelFileDescriptor;
import android.view.Surface;
import androidx.annotation.IntDef;
import androidx.annotation.Keep;
import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.annotation.RequiresApi;
import java.io.IOException;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

/**
 * Registry of backend textures used with a single {@link io.flutter.embedding.android.FlutterView}
 * instance. Entries may be embedded into the Flutter view using the <a
 * href="https://api.flutter.dev/flutter/widgets/Texture-class.html">Texture</a> widget.
 */
public interface TextureRegistry {
  /**
   * Creates and registers a {@link SurfaceProducer}, or a Flutter-managed {@link Surface}.
   *
   * <p>Uses the {@link SurfaceLifecycle#manual} lifecycle implicitly.
   *
   * @return A SurfaceProducer.
   */
  @NonNull
  default SurfaceProducer createSurfaceProducer() {
    return createSurfaceProducer(SurfaceLifecycle.manual);
  }
  /**
   * How a {@link SurfaceProducer} created by {@link #createSurfaceProducer()} manages the lifecycle
   * of the created surface.
   */
  enum SurfaceLifecycle {
    /**
     * The surface and latest image should be kept, even if the app enters the background.
     *
     * <p>The application, or calling code, can choose to (manually) reset the surface at the
     * appropriate time (such as to lower memory pressure, or cleanup an unused surface), but by
     * default the surface will never be reset, and as a result, new images do not have to be drawn
     * to the surface.
     *
     * <p>This is an appropriate lifecycle for external textures, as it is not guaranteed that new
     * images will be drawn to the surface, and whether the image should be kept when the app is
     * backgrounded.
     */
    manual,

    /**
     * The surface will be reset if the app enters the background.
     *
     * <p>While the application can choose to manually reset the surface, Flutter may automatically
     * reset the surface when the app enters the background. If the surface is reset, and no new
     * images are drawn to the surface, the texture will appear blank.
     *
     * <p>This is an appropriate lifecycle for platform views, as the platform implementation will
     * request a new surface, and draw to, as appropriate when resuming from the background, and
     * producing a new image when coming back to the foreground.
     */
    resetInBackground
  }

  /**
   * Creates and a {@link SurfaceProducer}, or a Flutter-managed {@link Surface}.
   *
   * @param lifecycle Whether to automatically reset the last image and release the surface.
   * @return A SurfaceProducer.
   */
  @NonNull
  SurfaceProducer createSurfaceProducer(SurfaceLifecycle lifecycle);

  /**
   * Creates and registers a SurfaceTexture managed by the Flutter engine.
   *
   * @return A SurfaceTextureEntry.
   */
  @NonNull
  SurfaceTextureEntry createSurfaceTexture();

  /**
   * Registers a SurfaceTexture managed by the Flutter engine.
   *
   * @return A SurfaceTextureEntry.
   */
  @NonNull
  SurfaceTextureEntry registerSurfaceTexture(@NonNull SurfaceTexture surfaceTexture);

  /**
   * Creates and registers a texture managed by the Flutter engine.
   *
   * @return a ImageTextureEntry.
   */
  @NonNull
  ImageTextureEntry createImageTexture();

  /**
   * Callback invoked when memory is low.
   *
   * <p>Invoke this from {@link android.app.Activity#onTrimMemory(int)}.
   */
  default void onTrimMemory(int level) {}

  /** An entry in the texture registry. */
  interface TextureEntry {
    /** @return The identity of this texture. */
    long id();

    /** De-registers and releases all resources . */
    void release();
  }

  /** Uses a Surface to populate the texture. */
  @Keep
  interface SurfaceProducer extends TextureEntry {
    /** Specify the size of this texture in physical pixels */
    void setSize(int width, int height);

    /** @return The currently specified width (physical pixels) */
    int getWidth();

    /** @return The currently specified height (physical pixels) */
    int getHeight();

    /**
     * Direct access to the surface object.
     *
     * <p>When using this API, you will usually need to implement {@link SurfaceProducer.Callback}
     * and provide it to {@link #setCallback(Callback)} in order to be notified when an existing
     * surface has been destroyed (such as when the application goes to the background) or a new
     * surface has been created (such as when the application is resumed back to the foreground).
     *
     * <p>NOTE: You should not cache the returned surface but instead invoke {@code getSurface} each
     * time you need to draw. The surface may change when the texture is resized or has its format
     * changed.
     *
     * @return a Surface to use for a drawing target for various APIs.
     */
    Surface getSurface();

    /**
     * Direct access to a surface, which will be newly created (and thus, different from any surface
     * objects returned from previous calls to {@link #getSurface()} or {@link
     * #getForcedNewSurface()}.
     *
     * <p>When using this API, you will usually need to implement {@link SurfaceProducer.Callback}
     * and provide it to {@link #setCallback(Callback)} in order to be notified when an existing
     * surface has been destroyed (such as when the application goes to the background) or a new
     * surface has been created (such as when the application is resumed back to the foreground).
     *
     * <p>NOTE: You should not cache the returned surface but instead invoke {@code getSurface} each
     * time you need to draw. The surface may change when the texture is resized or has its format
     * changed.
     *
     * @return a Surface to use for a drawing target for various APIs.
     */
    Surface getForcedNewSurface();

    /**
     * Sets a callback that is notified when a previously created {@link Surface} returned by {@link
     * SurfaceProducer#getSurface()} is no longer valid due to being destroyed, or a new surface is
     * now available (after the previous one was destroyed) for rendering.
     *
     * @param callback The callback to notify, or null to remove the callback.
     */
    void setCallback(Callback callback);

    /** Callback invoked by {@link #setCallback(Callback)}. */
    interface Callback {
      /**
       * An alias for {@link Callback#onSurfaceAvailable()} with a less accurate name.
       *
       * @deprecated Override and use {@link Callback#onSurfaceAvailable()} instead.
       */
      @Deprecated(since = "Flutter 3.27", forRemoval = true)
      default void onSurfaceCreated() {}

      /**
       * Invoked when an Android application is resumed after {@link Callback#onSurfaceDestroyed()}.
       *
       * <p>When this method is overridden, {@link Callback#onSurfaceCreated()} is not called.
       *
       * <p>Applications should now call {@link SurfaceProducer#getSurface()} to get a new
       * {@link Surface}, as the previous one was destroyed and released as a result of a low memory
       * event from the Android OS.
       *
       * <pre>
       * {@code
       * void example(SurfaceProducer producer) {
       *   producer.setCallback(new SurfaceProducer.Callback() {
       *     @override
       *     public void onSurfaceAvailable() {
       *       Surface surface = producer.getSurface();
       *       redrawOrUse(surface);
       *     }
       *
       *     // ...
       *   });
       * }
       * }
       * </pre>
       */
      default void onSurfaceAvailable() {
        this.onSurfaceCreated();
      }

      /**
       * An alias for {@link Callback#onSurfaceCleanup()} with a less accurate name.
       *
       * @deprecated Override and use {@link Callback#onSurfaceCleanup()} instead.
       */
      @Deprecated(since = "Flutter 3.28", forRemoval = true)
      default void onSurfaceDestroyed() {}

      /**
       * Invoked when a {@link Surface} returned by {@link SurfaceProducer#getSurface()} is about
       * to become invalid.
       *
       * <p>When this method is overridden, {@link Callback#onSurfaceDestroyed()} is not called.
       *
       * <p>In a low memory environment, the Android OS will signal to Flutter to release resources,
       * such as surfaces, that are not currently in use, such as when the application is in the
       * background, and this method is subsequently called to notify a plugin author to stop
       * using or rendering to the last surface.
       *
       * <p>Use {@link Callback#onSurfaceAvailable()} to be notified to resume rendering.
       *
       * <pre>
       * {@code
       * void example(SurfaceProducer producer) {
       *   producer.setCallback(new SurfaceProducer.Callback() {
       *     @override
       *     public void onSurfaceCleanup() {
       *       // Store information about the last frame, if necessary.
       *       // Potentially release other dependent resources.
       *     }
       *
       *     // ...
       *   });
       * }
       * }
       * </pre>
       */
      default void onSurfaceCleanup() {
        onSurfaceDestroyed();
      }
    }

    /** This method is not officially part of the public API surface and will be deprecated. */
    void scheduleFrame();

    /**
     * Returns whether the current rendering path handles crop and rotation metadata.
     *
     * <p>On most newer Android devices (API 29+), a {@link android.media.ImageReader} backend is
     * used, which has more features, works in new graphic backends directly (such as Impeller's
     * Vulkan backend), and is the Android recommended solution. However, crop and rotation metadata
     * are <strong>not</strong> handled automatically, and require plugin authors to make
     * appropriate changes ({@see https://github.com/flutter/flutter/issues/144407}).
     *
     * <pre>{@code
     * void example(SurfaceProducer producer) {
     *   bool supported = producer.handlesCropAndRotation();
     *   if (!supported) {
     *       // Manually rotate/crop, either in the Android plugin or in the Dart framework layer.
     *   }
     * }
     * }</pre>
     *
     * @return {@code true} if crop and rotation is handled automatically, {@code false} otherwise.
     */
    boolean handlesCropAndRotation();
  }

  /** A registry entry for a managed SurfaceTexture. */
  @Keep
  interface SurfaceTextureEntry extends TextureEntry {
    /** @return The managed SurfaceTexture. */
    @NonNull
    SurfaceTexture surfaceTexture();

    /** Set a listener that will be notified when the most recent image has been consumed. */
    default void setOnFrameConsumedListener(@Nullable OnFrameConsumedListener listener) {}

    /** Set a listener that will be notified when a memory pressure warning was forward. */
    default void setOnTrimMemoryListener(@Nullable OnTrimMemoryListener listener) {}
  }

  @Keep
  interface ImageTextureEntry extends TextureEntry {
    /**
     * Color space of a buffer passed to {@link #pushHardwareBuffer(long, int, int)}. Values match
     * {@code impeller::ColorSpace} (impeller/core/texture_descriptor.h). {@link
     * #COLOR_SPACE_UNSPECIFIED} lets the engine infer the color space from the buffer's pixel
     * format.
     */
    @IntDef({
      COLOR_SPACE_UNSPECIFIED,
      COLOR_SPACE_SRGB,
      COLOR_SPACE_DISPLAY_P3,
      COLOR_SPACE_LINEAR_DISPLAY_P3,
      COLOR_SPACE_LINEAR_P3_NATIVE
    })
    @Retention(RetentionPolicy.SOURCE)
    @interface ColorSpace {}

    int COLOR_SPACE_UNSPECIFIED = -1;
    int COLOR_SPACE_SRGB = 0;
    int COLOR_SPACE_DISPLAY_P3 = 1;
    int COLOR_SPACE_LINEAR_DISPLAY_P3 = 2;
    int COLOR_SPACE_LINEAR_P3_NATIVE = 3;

    /**
     * Next paint will update texture to use the contents of image.
     *
     * <p>NOTE: Caller should not call Image.close() on the pushed image.
     *
     * <p>NOTE: In the case that multiple calls to PushFrame occur before the next paint only the
     * last frame pushed will be used (dropping the missed frames).
     */
    void pushImage(Image image);

    /**
     * Direct-{@code AHardwareBuffer} variant of {@link #pushImage(Image)}.
     *
     * <p>Skips the {@link android.media.Image} / {@link android.hardware.HardwareBuffer} wrapping
     * overhead for producers that already manage {@code AHardwareBuffer}s directly (e.g. a Vulkan
     * renderer that rendered into an externally-imported {@code AHardwareBuffer} and wants to hand
     * off to the engine without an {@code ImageReader} queue sitting between them).
     *
     * <p>Ownership: caller MUST have called {@code AHardwareBuffer_acquire} on {@code ahbPtr}
     * immediately before this call. The engine takes ownership of that reference and releases it
     * when the buffer is composited or superseded by a later push. {@code acquireFenceFd} is dup'd
     * or closed by the engine — caller must not use it after this call. Pass {@code -1} if there is
     * no fence to wait on.
     *
     * <p>Mutually exclusive with {@link #pushImage(Image)} on a given entry; intermixing is
     * undefined.
     *
     * <p>Thread safety: unlike {@link #pushImage(Image)} which is {@code @UiThread}, this method is
     * safe to call from any thread. Direct-{@code AHardwareBuffer} producers commonly run on native
     * worker threads (Vulkan queue-submission callbacks, MediaCodec decoders, etc.). The state
     * update (stashing the new AHB, releasing any superseded one) runs synchronously on the
     * caller's thread; only the UI-thread-only {@code scheduleEngineFrame} follow-up is posted to
     * the main looper when called from a background thread.
     *
     * <p>Default implementation is a no-op so third-party {@code ImageTextureEntry} implementations
     * (rare in practice, but legal per the public API) keep compiling.
     *
     * @param ahbPtr native {@code AHardwareBuffer*} pointer. MUST be non-zero; implementations
     *     throw {@link IllegalArgumentException} on zero.
     * @param acquireFenceFd fence fd to wait on before reading, or {@code -1} when the caller has
     *     already synchronized on the CPU or GPU side.
     */
    @RequiresApi(API_LEVELS.API_26)
    default void pushHardwareBuffer(long ahbPtr, int acquireFenceFd) {
      pushHardwareBuffer(ahbPtr, acquireFenceFd, COLOR_SPACE_UNSPECIFIED);
    }

    /**
     * Variant of {@link #pushHardwareBuffer(long, int)} that additionally declares the buffer's
     * color space. The engine uses it to interpret the buffer's pixels at composite (gamut +
     * transfer) rather than inferring from the pixel format. Pass {@link #COLOR_SPACE_UNSPECIFIED}
     * to keep the format-based inference.
     *
     * <p>Color conversion is applied by the Impeller renderer only; the legacy Skia OpenGL ES
     * renderer samples the buffer's values as they are.
     */
    @RequiresApi(API_LEVELS.API_26)
    default void pushHardwareBuffer(long ahbPtr, int acquireFenceFd, @ColorSpace int colorSpace) {}

    /**
     * Sets a listener that is told when the engine is done with each buffer pushed via {@link
     * #pushHardwareBuffer(long, int)}, so a producer that reuses a pool of {@code AHardwareBuffer}s
     * knows when it may overwrite one. Set it before the first push: buffers pushed while no
     * listener is set are not reported, and the engine makes no release fences for them.
     *
     * <p>The listener is called once for every push, with a release fence the listener owns:
     *
     * <ul>
     *   <li>A push the engine never drew, because a later push, a {@link #pushImage(Image)} or
     *       {@link #release()} superseded it, is released at once with fence {@code -1},
     *       synchronously on the superseding thread, from inside that call. The listener must not
     *       take a lock the caller of {@code pushHardwareBuffer} holds.
     *   <li>A push the engine drew is released once a later frame replaces it on screen, after that
     *       frame has been submitted, on the raster thread. Its fence signals once the GPU has
     *       finished every draw that read it; it is {@code -1} when that work is already done. The
     *       listener must not block or use EGL/OpenGL state there.
     *   <li>The buffer on screen when the entry is released is reported, on the raster thread, when
     *       the engine destroys the texture, which happens after {@link #release()} returns. An
     *       entry that is garbage-collected without {@link #release()} does not report it.
     * </ul>
     *
     * <p>Releases are per push: a producer that pushes the buffer already on screen again gets one
     * release for each push, and may only overwrite the buffer once every push of it has been
     * released.
     *
     * <p>The listener owns each fence fd it is given, even if it throws; exceptions are caught and
     * logged.
     *
     * @param listener The listener to notify, or {@code null} to remove it.
     */
    @RequiresApi(API_LEVELS.API_26)
    default void setOnHardwareBufferReleasedListener(
        @Nullable OnHardwareBufferReleasedListener listener) {}
  }

  /**
   * Listener invoked when the engine is done with a buffer pushed via {@link
   * ImageTextureEntry#pushHardwareBuffer(long, int)}.
   */
  interface OnHardwareBufferReleasedListener {
    /**
     * Called once per pushed buffer, on an arbitrary thread (the raster thread for a buffer the
     * engine sampled). Must not block.
     *
     * @param ahbPtr The {@code AHardwareBuffer*} that was pushed. It identifies the buffer only:
     *     the engine has already dropped the reference the push transferred to it.
     * @param releaseFenceFd A sync fence fd that signals once the GPU has finished reading the
     *     buffer, or {@code -1} when it may be overwritten immediately. The listener owns the fd
     *     and must close it (or hand it to {@code vkImportSemaphoreFdKHR} / {@code
     *     eglCreateSyncKHR}, which take ownership).
     */
    void onHardwareBufferReleased(long ahbPtr, int releaseFenceFd);
  }

  /** Listener invoked when the most recent image has been consumed. */
  interface OnFrameConsumedListener {
    /**
     * This method will to be invoked when the most recent image from the image stream has been
     * consumed.
     */
    void onFrameConsumed();
  }

  /** Listener invoked when a memory pressure warning was forward. */
  interface OnTrimMemoryListener {
    /** This method will be invoked when a memory pressure warning was forward. */
    void onTrimMemory(int level);
  }

  @Keep
  interface ImageConsumer {
    /**
     * Retrieve the last Image produced. Drops all previously produced images.
     *
     * <p>NOTE: Caller must call Image.close() on returned image.
     *
     * @return Image or null.
     */
    @Nullable
    Image acquireLatestImage();

    /**
     * Direct-{@code AHardwareBuffer} companion to {@link #acquireLatestImage()}.
     *
     * <p>Retrieves the last raw {@code AHardwareBuffer} pushed via {@link
     * ImageTextureEntry#pushHardwareBuffer(long, int)} and its associated acquire fence fd.
     * Consumer takes ownership: it must release the {@code AHardwareBuffer} via {@code
     * AHardwareBuffer_release} once compositing is complete, and close the fence fd (or use it in a
     * {@code vkImportFence} / {@code vkImportSemaphoreFd}) before the corresponding rendering
     * dispatch.
     *
     * <p>Returns {@code null} if no {@code AHardwareBuffer} has been pushed, or if the entry is in
     * the legacy {@link Image}-based flow (see {@link #acquireLatestImage()}).
     *
     * @return {@link HardwareBufferHandle} or {@code null}.
     */
    @Nullable
    @RequiresApi(API_LEVELS.API_26)
    default HardwareBufferHandle acquireLatestHardwareBuffer() {
      return null;
    }

    /**
     * Called by the engine when it is done with a buffer returned by {@link
     * #acquireLatestHardwareBuffer()}. Takes ownership of {@code releaseFenceFd} ({@code -1} if
     * none). See {@link ImageTextureEntry#setOnHardwareBufferReleasedListener}.
     */
    @RequiresApi(API_LEVELS.API_26)
    default void onHardwareBufferReleased(long ahbPtr, int releaseFenceFd) {
      if (releaseFenceFd < 0) {
        return;
      }
      try {
        ParcelFileDescriptor.adoptFd(releaseFenceFd).close();
      } catch (IOException e) {
        // Nothing more can be done with a fence that fails to close.
      }
    }
  }

  /**
   * Plain-data handle to a raw {@code AHardwareBuffer} + its acquire fence, returned by {@link
   * ImageConsumer#acquireLatestHardwareBuffer()}.
   *
   * <p>Ownership: see that method's contract.
   */
  @Keep
  final class HardwareBufferHandle {
    /** Native {@code AHardwareBuffer*} pointer. Always non-zero when this handle is returned. */
    public final long ahbPtr;
    /** Acquire fence fd, or {@code -1} when there is no fence to wait on. */
    public final int acquireFenceFd;
    /**
     * Color-space code (one of {@link ImageTextureEntry}'s {@code COLOR_SPACE_*}) the producer
     * declared for this buffer, or {@link ImageTextureEntry#COLOR_SPACE_UNSPECIFIED} to infer from
     * the format.
     */
    @ImageTextureEntry.ColorSpace public final int colorSpace;
    /**
     * Whether the producer listens for this buffer's release. When {@code false} the engine makes
     * no release fence for it and does not report its release.
     */
    public final boolean wantsRelease;

    public HardwareBufferHandle(
        long ahbPtr,
        int acquireFenceFd,
        @ImageTextureEntry.ColorSpace int colorSpace,
        boolean wantsRelease) {
      this.ahbPtr = ahbPtr;
      this.acquireFenceFd = acquireFenceFd;
      this.colorSpace = colorSpace;
      this.wantsRelease = wantsRelease;
    }
  }

  @Keep
  interface GLTextureConsumer {
    /**
     * Retrieve the last GL texture produced.
     *
     * @return SurfaceTexture.
     */
    @NonNull
    SurfaceTexture getSurfaceTexture();
  }
}
