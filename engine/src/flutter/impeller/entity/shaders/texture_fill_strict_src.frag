// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

precision mediump float;

#include <impeller/color.glsl>
#include <impeller/constants.glsl>
#include <impeller/types.glsl>

uniform f16sampler2D texture_sampler;

uniform FragInfo {
  vec4 source_rect;
  float alpha;
  float gamma_encode;
  vec3 color_row0;
  vec3 color_row1;
  vec3 color_row2;
}
frag_info;

in highp vec2 v_texture_coords;

out f16vec4 frag_color;

void main() {
  vec2 texture_coords = vec2(clamp(v_texture_coords.x, frag_info.source_rect.x,
                                   frag_info.source_rect.z),
                             clamp(v_texture_coords.y, frag_info.source_rect.y,
                                   frag_info.source_rect.w));
  f16vec4 sampled =
      texture(texture_sampler, texture_coords, float16_t(kDefaultMipBias));
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
