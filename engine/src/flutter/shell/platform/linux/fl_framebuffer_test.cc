// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gtest/gtest.h"

#include "flutter/shell/platform/linux/fl_framebuffer.h"
#include "flutter/shell/platform/linux/testing/mock_epoxy.h"

TEST(FlFramebufferTest, HasDepthStencil) {
  ::testing::NiceMock<flutter::testing::MockEpoxy> epoxy;

  g_autoptr(FlFramebuffer) framebuffer =
      fl_framebuffer_new(GL_RGB, 100, 100, FALSE);

  GLint depth_type = GL_NONE;
  glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,
                                        &depth_type);
  EXPECT_NE(depth_type, GL_NONE);

  GLint stencil_type = GL_NONE;
  glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                                        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,
                                        &stencil_type);
  EXPECT_NE(stencil_type, GL_NONE);
}

TEST(FlFramebufferTest, ResourcesRemoved) {
  ::testing::NiceMock<flutter::testing::MockEpoxy> epoxy;

  EXPECT_CALL(epoxy, glGenFramebuffers);
  EXPECT_CALL(epoxy, glGenTextures);
  EXPECT_CALL(epoxy, glGenRenderbuffers);
  FlFramebuffer* framebuffer = fl_framebuffer_new(GL_RGB, 100, 100, FALSE);

  EXPECT_CALL(epoxy, glDeleteFramebuffers);
  EXPECT_CALL(epoxy, glDeleteTextures);
  EXPECT_CALL(epoxy, glDeleteRenderbuffers);
  g_object_unref(framebuffer);
}

// Regression test: during shutdown the window (and its EGL context) can be
// destroyed before the compositor tears down its framebuffers. Disposing a
// framebuffer with no current context must NOT issue gl* deletes, because epoxy
// aborts when it cannot resolve a GL entry point without a current context.
TEST(FlFramebufferTest, ResourcesRetainedWithoutContext) {
  ::testing::NiceMock<flutter::testing::MockEpoxy> epoxy;

  EXPECT_CALL(epoxy, glGenFramebuffers);
  EXPECT_CALL(epoxy, glGenTextures);
  EXPECT_CALL(epoxy, glGenRenderbuffers);
  FlFramebuffer* framebuffer = fl_framebuffer_new(GL_RGB, 100, 100, FALSE);

  // Simulate the context being gone by the time the framebuffer is disposed.
  EXPECT_CALL(epoxy, eglGetCurrentContext)
      .WillRepeatedly(::testing::Return(EGL_NO_CONTEXT));
  EXPECT_CALL(epoxy, glDeleteFramebuffers).Times(0);
  EXPECT_CALL(epoxy, glDeleteTextures).Times(0);
  EXPECT_CALL(epoxy, glDeleteRenderbuffers).Times(0);
  g_object_unref(framebuffer);
}

// F16 (HDR) backing stores allocate as sized GL_RGBA16F with GL_HALF_FLOAT
// pixel-transfer type — GL_RGBA16F is a sized internal format and cannot
// double as the transfer format the way GL_RGBA does on the 8-bit path.
TEST(FlFramebufferTest, F16Allocation) {
  ::testing::NiceMock<flutter::testing::MockEpoxy> epoxy;

  EXPECT_CALL(epoxy, glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, 100, 100, 0,
                                  GL_RGBA, GL_HALF_FLOAT, nullptr));
  g_autoptr(FlFramebuffer) framebuffer =
      fl_framebuffer_new(GL_RGBA16F, 100, 100, FALSE);
}

// The 8-bit path is unchanged by the F16 support: unsized format doubles as
// the transfer format with GL_UNSIGNED_BYTE.
TEST(FlFramebufferTest, Rgba8Allocation) {
  ::testing::NiceMock<flutter::testing::MockEpoxy> epoxy;

  EXPECT_CALL(epoxy, glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 100, 100, 0,
                                  GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
  g_autoptr(FlFramebuffer) framebuffer =
      fl_framebuffer_new(GL_RGBA, 100, 100, FALSE);
}

TEST(FlFramebufferTest, Sibling) {
  ::testing::NiceMock<flutter::testing::MockEpoxy> epoxy;

  EXPECT_CALL(epoxy, eglCreateImageKHR);
  g_autoptr(FlFramebuffer) framebuffer =
      fl_framebuffer_new(GL_RGB, 100, 100, TRUE);
  g_autoptr(FlFramebuffer) sibling = fl_framebuffer_create_sibling(framebuffer);
}
