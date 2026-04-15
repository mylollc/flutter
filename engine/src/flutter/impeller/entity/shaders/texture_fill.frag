// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

precision mediump float;

#include <impeller/color.glsl>
#include <impeller/constants.glsl>
#include <impeller/types.glsl>

uniform f16sampler2D texture_sampler;

uniform FragInfo {
  float alpha;
  // 1.0 when source texture is linear and needs sRGB gamma encoding.
  // 0.0 when source is already gamma-encoded (default for most textures).
  float gamma_encode;
  // Rows of a 3x3 gamut conversion matrix. Identity (1,0,0 / 0,1,0 / 0,0,1)
  // when source and destination color spaces match. Set to the P3→sRGB matrix
  // when drawing Display P3 content into an sRGB surface.
  vec3 color_row0;
  vec3 color_row1;
  vec3 color_row2;
}
frag_info;

in highp vec2 v_texture_coords;

out f16vec4 frag_color;

void main() {
  f16vec4 sampled =
      texture(texture_sampler, v_texture_coords, float16_t(kDefaultMipBias));
  vec3 rgb = vec3(sampled.rgb);
  rgb = vec3(dot(frag_info.color_row0, rgb),
             dot(frag_info.color_row1, rgb),
             dot(frag_info.color_row2, rgb));
  if (frag_info.gamma_encode > 0.5) {
    rgb = IPSrgbOETF(rgb);
  }
  sampled.rgb = f16vec3(rgb);
  frag_color = sampled * float16_t(frag_info.alpha);
}
