#version 320 es

// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Test shader with >31 float uniforms to verify Metal UBO wrapping.
// Metal has a 31 buffer binding limit. Without UBO wrapping, each uniform
// gets its own [[buffer(N)]] and the shader fails to compile on iOS.
// With UBO wrapping, all floats are packed into a single struct at buffer(0).

precision highp float;

#include <flutter/runtime_effect.glsl>

uniform float u_00;
uniform float u_01;
uniform float u_02;
uniform float u_03;
uniform float u_04;
uniform float u_05;
uniform float u_06;
uniform float u_07;
uniform float u_08;
uniform float u_09;
uniform float u_10;
uniform float u_11;
uniform float u_12;
uniform float u_13;
uniform float u_14;
uniform float u_15;
uniform float u_16;
uniform float u_17;
uniform float u_18;
uniform float u_19;
uniform float u_20;
uniform float u_21;
uniform float u_22;
uniform float u_23;
uniform float u_24;
uniform float u_25;
uniform float u_26;
uniform float u_27;
uniform float u_28;
uniform float u_29;
uniform float u_30;
uniform float u_31;
uniform float u_32;
uniform float u_33;
uniform float u_34;
uniform sampler2D u_texture;

layout(location = 0) out vec4 fragColor;

void main() {
  vec2 uv = FlutterFragCoord().xy;
  float sum = u_00 + u_01 + u_02 + u_03 + u_04 + u_05 + u_06 + u_07 +
              u_08 + u_09 + u_10 + u_11 + u_12 + u_13 + u_14 + u_15 +
              u_16 + u_17 + u_18 + u_19 + u_20 + u_21 + u_22 + u_23 +
              u_24 + u_25 + u_26 + u_27 + u_28 + u_29 + u_30 + u_31 +
              u_32 + u_33 + u_34;
  fragColor = texture(u_texture, uv) * sum;
}
