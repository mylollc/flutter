// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/impeller/geometry/geometry_asserts.h"
#include "impeller/entity/contents/texture_contents.h"
#include "third_party/googletest/googletest/include/gtest/gtest.h"

namespace impeller {
namespace testing {

namespace {

constexpr Vector3 kIdentityRow0 = {1.0f, 0.0f, 0.0f};
constexpr Vector3 kIdentityRow1 = {0.0f, 1.0f, 0.0f};
constexpr Vector3 kIdentityRow2 = {0.0f, 0.0f, 1.0f};

// Applies the gamut rows of `transform` to `rgb`.
Vector3 Convert(const TextureColorTransform& transform, Vector3 rgb) {
  return {transform.row0.Dot(rgb), transform.row1.Dot(rgb),
          transform.row2.Dot(rgb)};
}

}  // namespace

TEST(TextureColorTransformTest, SRGBIsDrawnAsIs) {
  const TextureColorTransform transform =
      GetTextureColorTransform(ColorSpace::kSRGB);
  EXPECT_VECTOR3_NEAR(transform.row0, kIdentityRow0);
  EXPECT_VECTOR3_NEAR(transform.row1, kIdentityRow1);
  EXPECT_VECTOR3_NEAR(transform.row2, kIdentityRow2);
  EXPECT_FALSE(transform.gamma_encode);
}

TEST(TextureColorTransformTest, DisplayP3IsConvertedToSRGBPrimaries) {
  const TextureColorTransform transform =
      GetTextureColorTransform(ColorSpace::kDisplayP3);
  // Already gamma encoded.
  EXPECT_FALSE(transform.gamma_encode);
  // White stays white; pure P3 red lies outside sRGB.
  EXPECT_VECTOR3_NEAR(Convert(transform, {1.0f, 1.0f, 1.0f}),
                      Vector3(1.0f, 1.0f, 1.0f));
  const Vector3 red = Convert(transform, {1.0f, 0.0f, 0.0f});
  EXPECT_GT(red.x, 1.0f);
  EXPECT_LT(red.y, 0.0f);
}

TEST(TextureColorTransformTest, LinearDisplayP3IsConvertedAndEncoded) {
  const TextureColorTransform transform =
      GetTextureColorTransform(ColorSpace::kLinearDisplayP3);
  const TextureColorTransform gamma_p3 =
      GetTextureColorTransform(ColorSpace::kDisplayP3);
  EXPECT_TRUE(transform.gamma_encode);
  EXPECT_VECTOR3_NEAR(transform.row0, gamma_p3.row0);
  EXPECT_VECTOR3_NEAR(transform.row1, gamma_p3.row1);
  EXPECT_VECTOR3_NEAR(transform.row2, gamma_p3.row2);
}

TEST(TextureColorTransformTest, LinearP3NativeIsOnlyEncoded) {
  const TextureColorTransform transform =
      GetTextureColorTransform(ColorSpace::kLinearP3Native);
  EXPECT_TRUE(transform.gamma_encode);
  EXPECT_VECTOR3_NEAR(transform.row0, kIdentityRow0);
  EXPECT_VECTOR3_NEAR(transform.row1, kIdentityRow1);
  EXPECT_VECTOR3_NEAR(transform.row2, kIdentityRow2);
}

}  // namespace testing
}  // namespace impeller
