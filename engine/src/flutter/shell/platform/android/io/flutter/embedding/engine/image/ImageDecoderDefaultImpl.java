// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package io.flutter.embedding.engine.image;

import android.graphics.Bitmap;
import android.graphics.ColorSpace;
import android.util.Size;
import androidx.annotation.RequiresApi;
import io.flutter.Log;
import java.io.IOException;
import java.nio.ByteBuffer;

/**
 * The default implementation of {@link ImageDecoder} that uses {@link
 * android.graphics.ImageDecoder} to decode images.
 */
@RequiresApi(io.flutter.Build.API_LEVELS.API_28)
class ImageDecoderDefaultImpl implements ImageDecoder {
  private static final String TAG = "FlutterImageDecoderImplDefault";
  private final FlutterImageDecoder.HeaderListener listener;

  /**
   * Constructs a new {@code FlutterImageDecoderImplDefault}.
   *
   * @param listener A listener to receive image header information.
   */
  public ImageDecoderDefaultImpl(FlutterImageDecoder.HeaderListener listener) {
    this.listener = listener;
  }

  /**
   * Decodes an image from the given {@link ByteBuffer}.
   *
   * @param buffer The {@link ByteBuffer} containing the encoded image.
   * @param metadata The metadata of the image. This is unused here.
   * @return The decoded {@link Bitmap}, or null if decoding fails.
   */
  public Bitmap decodeImage(ByteBuffer buffer, Metadata metadata) {
    android.graphics.ImageDecoder.Source source =
        android.graphics.ImageDecoder.createSource(buffer);
    try {
      Bitmap bitmap =
          android.graphics.ImageDecoder.decodeBitmap(
              source,
              (decoder, info, src) -> {
                // Request sRGB output. For most sources this produces an
                // ARGB_8888 bitmap. For HDR sources (PQ/HLG HEIC, AVIF),
                // `ImageDecoder` may still return an `RGBA_F16` bitmap
                // because sRGB 8-bit can't faithfully represent the source
                // — the target color space is treated as a hint, not a
                // hard constraint on bit depth.
                decoder.setTargetColorSpace(ColorSpace.get(ColorSpace.Named.SRGB));
                // TODO(bdero): Switch to ALLOCATOR_HARDWARE for devices that have
                // `AndroidBitmap_getHardwareBuffer` (API 30+) available once Skia supports
                // `SkImage::MakeFromAHardwareBuffer` via dynamic lookups:
                // https://skia-review.googlesource.com/c/skia/+/428960
                decoder.setAllocator(android.graphics.ImageDecoder.ALLOCATOR_SOFTWARE);

                if (listener != null) {
                  Size size = info.getSize();
                  listener.onImageHeader(size.getWidth(), size.getHeight());
                }
              });

      // Downstream (`android_image_generator.cc::GetPixels`) DCHECKs that
      // the bitmap format is ANDROID_BITMAP_FORMAT_RGBA_8888. HDR sources
      // can land here as `RGBA_F16` or `RGBA_1010102`; convert them to
      // `ARGB_8888` so the engine path only ever sees the format its
      // DCHECK expects. Lossy for HDR content (the F16 precision is
      // clamped to 8-bit sRGB) but this path is explicitly the SDR path —
      // any caller wanting HDR should go through the native renderer
      // pipeline, not the Dart image decoder.
      if (bitmap != null && bitmap.getConfig() != Bitmap.Config.ARGB_8888) {
        Bitmap converted = bitmap.copy(Bitmap.Config.ARGB_8888, false);
        if (converted == null) {
          Log.e(
              TAG,
              "Bitmap.copy to ARGB_8888 returned null for "
                  + bitmap.getConfig()
                  + " source; engine downstream expects ARGB_8888.");
          bitmap.recycle();
          return null;
        }
        bitmap.recycle();
        bitmap = converted;
      }
      return bitmap;
    } catch (IOException e) {
      Log.e(TAG, "Failed to decode image", e);
      return null;
    }
  }
}
