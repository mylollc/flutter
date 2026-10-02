// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

precision mediump float;

#include <impeller/color.glsl>
#include <impeller/external_texture_oes.glsl>

uniform sampler2D SAMPLER_EXTERNAL_OES_texture_sampler;

uniform FragInfo {
  float x_tile_mode;
  float y_tile_mode;
  float alpha;
  // 1.0 when source texture is linear and needs sRGB gamma encoding.
  // 0.0 when source is already gamma-encoded (default for most textures).
  float gamma_encode;
  // Rows of a 3x3 gamut conversion matrix, as in texture_fill.frag. Identity
  // when source and destination color spaces match.
  vec3 color_row0;
  vec3 color_row1;
  vec3 color_row2;
}
frag_info;

in highp vec2 v_texture_coords;

out vec4 frag_color;

void main() {
  vec4 sampled =
      IPSampleWithTileModeOES(SAMPLER_EXTERNAL_OES_texture_sampler,  // sampler
                              v_texture_coords,       // texture coordinates
                              frag_info.x_tile_mode,  // x tile mode
                              frag_info.y_tile_mode   // y tile mode
      );
  vec3 rgb = sampled.rgb;
  rgb = vec3(dot(frag_info.color_row0, rgb), dot(frag_info.color_row1, rgb),
             dot(frag_info.color_row2, rgb));
  if (frag_info.gamma_encode > 0.5) {
    rgb = IPSrgbOETF(rgb);
  }
  sampled.rgb = rgb;
  frag_color = sampled * frag_info.alpha;
}
