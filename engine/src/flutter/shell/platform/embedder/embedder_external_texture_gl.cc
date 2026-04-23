// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/embedder/embedder_external_texture_gl.h"

#include "flutter/fml/logging.h"
#include "impeller/core/texture_descriptor.h"
#include "impeller/display_list/aiks_context.h"
#include "impeller/display_list/dl_image_impeller.h"
#include "impeller/geometry/size.h"
#include "impeller/renderer/backend/gles/context_gles.h"
#include "impeller/renderer/backend/gles/handle_gles.h"
#include "impeller/renderer/backend/gles/texture_gles.h"

#include "include/core/SkPaint.h"
#include "third_party/skia/include/core/SkAlphaType.h"
#include "third_party/skia/include/core/SkColorSpace.h"
#include "third_party/skia/include/core/SkColorType.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkSize.h"
#include "third_party/skia/include/gpu/ganesh/GrBackendSurface.h"
#include "third_party/skia/include/gpu/ganesh/GrDirectContext.h"
#include "third_party/skia/include/gpu/ganesh/SkImageGanesh.h"
#include "third_party/skia/include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "third_party/skia/include/gpu/ganesh/gl/GrGLTypes.h"

// GL_RGBA16F_EXT is defined in GLES2/gl2ext.h (0x881A). Reported by the
// embedder's external texture callback for FP16 textures; used here to pick
// kRGBA_F16_SkColorType so Skia doesn't reject the HDR texture with a
// color-type mismatch.
#ifndef GL_RGBA16F_EXT
#define GL_RGBA16F_EXT 0x881A
#endif

namespace flutter {

EmbedderExternalTextureGL::EmbedderExternalTextureGL(
    int64_t texture_identifier,
    const ExternalTextureCallback& callback)
    : Texture(texture_identifier), external_texture_callback_(callback) {
  FML_DCHECK(external_texture_callback_);
}

EmbedderExternalTextureGL::~EmbedderExternalTextureGL() = default;

// |flutter::Texture|
void EmbedderExternalTextureGL::Paint(PaintContext& context,
                                      const DlRect& bounds,
                                      bool freeze,
                                      const DlImageSampling sampling) {
  if (last_image_ == nullptr) {
    last_image_ =
        ResolveTexture(Id(),                                                 //
                       context.gr_context,                                   //
                       context.aiks_context,                                 //
                       SkISize::Make(bounds.GetWidth(), bounds.GetHeight())  //
        );
  }

  DlCanvas* canvas = context.canvas;
  const DlPaint* paint = context.paint;

  if (last_image_) {
    DlRect image_bounds = DlRect::Make(last_image_->GetBounds());
    if (bounds != image_bounds) {
      canvas->DrawImageRect(last_image_, image_bounds, bounds, sampling, paint);
    } else {
      canvas->DrawImage(last_image_, bounds.GetOrigin(), sampling, paint);
    }
  }
}

sk_sp<DlImage> EmbedderExternalTextureGL::ResolveTexture(
    int64_t texture_id,
    GrDirectContext* context,
    impeller::AiksContext* aiks_context,
    const SkISize& size) {
  if (!!aiks_context) {
    return ResolveTextureImpeller(texture_id, aiks_context, size);
  } else {
    return ResolveTextureSkia(texture_id, context, size);
  }
}

sk_sp<DlImage> EmbedderExternalTextureGL::ResolveTextureSkia(
    int64_t texture_id,
    GrDirectContext* context,
    const SkISize& size) {
  context->flushAndSubmit();
  context->resetContext(kAll_GrBackendState);
  std::unique_ptr<FlutterOpenGLTexture> texture =
      external_texture_callback_(texture_id, size.width(), size.height());

  if (!texture) {
    return nullptr;
  }

  GrGLTextureInfo gr_texture_info = {texture->target, texture->name,
                                     texture->format};

  size_t width = size.width();
  size_t height = size.height();

  if (texture->width != 0 && texture->height != 0) {
    width = texture->width;
    height = texture->height;
  }

  auto gr_backend_texture = GrBackendTextures::MakeGL(
      width, height, skgpu::Mipmapped::kNo, gr_texture_info);
  SkImages::TextureReleaseProc release_proc = texture->destruction_callback;
  const bool is_f16 = (texture->format == GL_RGBA16F_EXT);
  const SkColorType color_type =
      is_f16 ? kRGBA_F16_SkColorType : kRGBA_8888_SkColorType;
  // F16 external textures carry linear scRGB pixel values (HDR content
  // pre-linearized by the embedder). Tagging them with MakeSRGBLinear() tells
  // Skia the values are already in the destination's linear color space so
  // compositing into an F16 linear-scRGB surface preserves >1.0 highlights
  // instead of running an sRGB->linear conversion that clamps them. 8-bit
  // textures pass nullptr (implicit sRGB), preserving prior behavior.
  sk_sp<SkColorSpace> color_space =
      is_f16 ? SkColorSpace::MakeSRGBLinear() : nullptr;
  auto image =
      SkImages::BorrowTextureFrom(context,                   // context
                                  gr_backend_texture,        // texture handle
                                  kTopLeft_GrSurfaceOrigin,  // origin
                                  color_type,                // color type
                                  kPremul_SkAlphaType,       // alpha type
                                  color_space,               // colorspace
                                  release_proc,       // texture release proc
                                  texture->user_data  // texture release context
      );

  if (!image) {
    // In case Skia rejects the image, call the release proc so that
    // embedders can perform collection of intermediates.
    if (release_proc) {
      release_proc(texture->user_data);
    }
    FML_LOG(ERROR) << "Could not create external texture->";
    return nullptr;
  }

  // This image should not escape local use by EmbedderExternalTextureGL
  return DlImage::Make(std::move(image));
}

sk_sp<DlImage> EmbedderExternalTextureGL::ResolveTextureImpeller(
    int64_t texture_id,
    impeller::AiksContext* aiks_context,
    const SkISize& size) {
  std::unique_ptr<FlutterOpenGLTexture> texture =
      external_texture_callback_(texture_id, size.width(), size.height());

  if (!texture) {
    return nullptr;
  }

  impeller::TextureDescriptor desc;
  desc.size = impeller::ISize(texture->width, texture->height);
  // TODO(upstream): propagate texture->format to desc.format to support FP16
  // external textures on the Impeller path, mirroring the Skia path above.
  // Not exercised by any current desktop embedder (Windows/macOS desktop use
  // Skia); unchanged here to avoid shipping unvalidated code.

  impeller::ContextGLES& context =
      impeller::ContextGLES::Cast(*aiks_context->GetContext());
  impeller::HandleGLES handle = context.GetReactor()->CreateHandle(
      impeller::HandleType::kTexture, texture->target);
  std::shared_ptr<impeller::TextureGLES> image =
      impeller::TextureGLES::WrapTexture(context.GetReactor(), desc, handle);

  if (!image) {
    // In case Skia rejects the image, call the release proc so that
    // embedders can perform collection of intermediates.
    if (texture->destruction_callback) {
      texture->destruction_callback(texture->user_data);
    }
    FML_LOG(ERROR) << "Could not create external texture";
    return nullptr;
  }
  if (texture->destruction_callback &&
      !context.GetReactor()->RegisterCleanupCallback(
          handle,
          [callback = texture->destruction_callback,
           user_data = texture->user_data]() { callback(user_data); })) {
    FML_LOG(ERROR) << "Could not register destruction callback";
    return nullptr;
  }

  return impeller::DlImageImpeller::Make(image);
}

// |flutter::Texture|
void EmbedderExternalTextureGL::OnGrContextCreated() {}

// |flutter::Texture|
void EmbedderExternalTextureGL::OnGrContextDestroyed() {}

// |flutter::Texture|
void EmbedderExternalTextureGL::MarkNewFrameAvailable() {
  last_image_ = nullptr;
}

// |flutter::Texture|
void EmbedderExternalTextureGL::OnTextureUnregistered() {}

}  // namespace flutter
