// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <vector>

#include "flutter/impeller/renderer/backend/gles/gles.h"
#include "flutter/shell/platform/windows/compositor_opengl.h"
#include "flutter/shell/platform/windows/egl/manager.h"
#include "flutter/shell/platform/windows/flutter_windows_view.h"
#include "flutter/shell/platform/windows/testing/egl/mock_context.h"
#include "flutter/shell/platform/windows/testing/egl/mock_manager.h"
#include "flutter/shell/platform/windows/testing/egl/mock_window_surface.h"
#include "flutter/shell/platform/windows/testing/engine_modifier.h"
#include "flutter/shell/platform/windows/testing/flutter_windows_engine_builder.h"
#include "flutter/shell/platform/windows/testing/mock_window_binding_handler.h"
#include "flutter/shell/platform/windows/testing/view_modifier.h"
#include "flutter/shell/platform/windows/testing/windows_test.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace flutter {
namespace testing {

namespace {
using ::testing::AnyNumber;
using ::testing::Return;

void MockGetIntegerv(GLenum name, int* value) {
  if (name == GL_NUM_EXTENSIONS) {
    *value = 1;
  } else {
    *value = 0;
  }
}

const unsigned char* MockGetString(GLenum name) {
  switch (name) {
    case GL_VERSION:
    case GL_SHADING_LANGUAGE_VERSION:
      return reinterpret_cast<const unsigned char*>("3.0");
    default:
      return reinterpret_cast<const unsigned char*>("");
  }
}

const unsigned char* MockGetStringi(GLenum name, int index) {
  if (name == GL_EXTENSIONS) {
    return reinterpret_cast<const unsigned char*>("GL_ANGLE_framebuffer_blit");
  } else {
    return reinterpret_cast<const unsigned char*>("");
  }
}

GLenum MockGetError() {
  return GL_NO_ERROR;
}

void DoNothing() {}

const impeller::ProcTableGLES::Resolver kMockResolver = [](const char* name) {
  std::string function_name{name};

  if (function_name == "glGetString") {
    return reinterpret_cast<void*>(&MockGetString);
  } else if (function_name == "glGetStringi") {
    return reinterpret_cast<void*>(&MockGetStringi);
  } else if (function_name == "glGetIntegerv") {
    return reinterpret_cast<void*>(&MockGetIntegerv);
  } else if (function_name == "glGetError") {
    return reinterpret_cast<void*>(&MockGetError);
  } else {
    return reinterpret_cast<void*>(&DoNothing);
  }
};

class CompositorOpenGLTest : public WindowsTest {
 public:
  CompositorOpenGLTest() = default;
  virtual ~CompositorOpenGLTest() = default;

 protected:
  FlutterWindowsEngine* engine() { return engine_.get(); }
  FlutterWindowsView* view() { return view_.get(); }
  egl::MockManager* egl_manager() { return egl_manager_; }
  egl::MockContext* render_context() { return render_context_.get(); }
  egl::MockWindowSurface* surface() { return surface_; }

  void UseHeadlessEngine() {
    auto egl_manager = std::make_unique<egl::MockManager>();
    render_context_ = std::make_unique<egl::MockContext>();
    egl_manager_ = egl_manager.get();

    EXPECT_CALL(*egl_manager_, render_context)
        .Times(AnyNumber())
        .WillRepeatedly(Return(render_context_.get()));
    EXPECT_CALL(*egl_manager_, is_rgba16float)
        .Times(AnyNumber())
        .WillRepeatedly(Return(false));

    FlutterWindowsEngineBuilder builder{GetContext()};

    engine_ = builder.Build();
    EngineModifier modifier{engine_.get()};
    modifier.SetEGLManager(std::move(egl_manager));
  }

  void UseEngineWithView(bool add_surface = true) {
    UseHeadlessEngine();

    auto window = std::make_unique<MockWindowBindingHandler>();
    EXPECT_CALL(*window.get(), SetView).Times(1);
    EXPECT_CALL(*window.get(), GetWindowHandle).WillRepeatedly(Return(nullptr));

    view_ = std::make_unique<FlutterWindowsView>(kImplicitViewId, engine_.get(),
                                                 std::move(window));

    if (add_surface) {
      auto surface = std::make_unique<egl::MockWindowSurface>();
      surface_ = surface.get();

      EXPECT_CALL(*surface_, Destroy).Times(AnyNumber());

      ViewModifier modifier{view_.get()};
      modifier.SetSurface(std::move(surface));
    }
  }

 private:
  std::unique_ptr<FlutterWindowsEngine> engine_;
  std::unique_ptr<FlutterWindowsView> view_;
  std::unique_ptr<egl::MockContext> render_context_;
  egl::MockWindowSurface* surface_;
  egl::MockManager* egl_manager_;

  FML_DISALLOW_COPY_AND_ASSIGN(CompositorOpenGLTest);
};

}  // namespace

TEST_F(CompositorOpenGLTest, CreateBackingStore) {
  UseHeadlessEngine();

  auto compositor =
      CompositorOpenGL{engine(), kMockResolver, /*enable_impeller=*/false};

  FlutterBackingStoreConfig config = {};
  FlutterBackingStore backing_store = {};

  EXPECT_CALL(*render_context(), MakeCurrent).WillOnce(Return(true));
  ASSERT_TRUE(compositor.CreateBackingStore(config, &backing_store));
  ASSERT_TRUE(compositor.CollectBackingStore(&backing_store));
}

TEST_F(CompositorOpenGLTest, CreateBackingStoreImpeller) {
  UseHeadlessEngine();

  auto compositor =
      CompositorOpenGL{engine(), kMockResolver, /*enable_impeller=*/true};

  FlutterBackingStoreConfig config = {};
  FlutterBackingStore backing_store = {};

  EXPECT_CALL(*render_context(), MakeCurrent).WillOnce(Return(true));
  ASSERT_TRUE(compositor.CreateBackingStore(config, &backing_store));
  ASSERT_TRUE(compositor.CollectBackingStore(&backing_store));
}

TEST_F(CompositorOpenGLTest, InitializationFailure) {
  UseHeadlessEngine();

  auto compositor =
      CompositorOpenGL{engine(), kMockResolver, /*enable_impeller=*/false};

  FlutterBackingStoreConfig config = {};
  FlutterBackingStore backing_store = {};

  EXPECT_CALL(*render_context(), MakeCurrent).WillOnce(Return(false));
  EXPECT_FALSE(compositor.CreateBackingStore(config, &backing_store));
}

TEST_F(CompositorOpenGLTest, InitializationRequiresBlit) {
  UseHeadlessEngine();

  const impeller::ProcTableGLES::Resolver resolver = [](const char* name) {
    std::string function_name{name};

    if (function_name == "glBlitFramebuffer" ||
        function_name == "glBlitFramebufferANGLE") {
      return (void*)nullptr;
    }

    return kMockResolver(name);
  };

  auto compositor =
      CompositorOpenGL{engine(), resolver, /*enable_impeller=*/false};

  FlutterBackingStoreConfig config = {};
  FlutterBackingStore backing_store = {};

  EXPECT_CALL(*render_context(), MakeCurrent).WillOnce(Return(true));
  ASSERT_FALSE(compositor.CreateBackingStore(config, &backing_store));
}

TEST_F(CompositorOpenGLTest, Present) {
  UseEngineWithView();

  auto compositor =
      CompositorOpenGL{engine(), kMockResolver, /*enable_impeller=*/false};

  FlutterBackingStoreConfig config = {};
  FlutterBackingStore backing_store = {};

  EXPECT_CALL(*render_context(), MakeCurrent).WillOnce(Return(true));
  ASSERT_TRUE(compositor.CreateBackingStore(config, &backing_store));

  FlutterLayer layer = {};
  layer.type = kFlutterLayerContentTypeBackingStore;
  layer.backing_store = &backing_store;
  const FlutterLayer* layer_ptr = &layer;

  EXPECT_CALL(*surface(), IsValid).WillRepeatedly(Return(true));
  EXPECT_CALL(*surface(), MakeCurrent).WillOnce(Return(true));
  EXPECT_CALL(*surface(), SwapBuffers).WillOnce(Return(true));
  EXPECT_TRUE(compositor.Present(view(), &layer_ptr, 1));

  ASSERT_TRUE(compositor.CollectBackingStore(&backing_store));
}

TEST_F(CompositorOpenGLTest, PresentEmpty) {
  UseEngineWithView();

  auto compositor =
      CompositorOpenGL{engine(), kMockResolver, /*enable_impeller=*/false};

  // The context will be bound twice: first to initialize the compositor, second
  // to clear the surface.
  EXPECT_CALL(*render_context(), MakeCurrent).WillOnce(Return(true));
  EXPECT_CALL(*surface(), IsValid).WillRepeatedly(Return(true));
  EXPECT_CALL(*surface(), MakeCurrent).WillOnce(Return(true));
  EXPECT_CALL(*surface(), SwapBuffers).WillOnce(Return(true));
  EXPECT_TRUE(compositor.Present(view(), nullptr, 0));
}

TEST_F(CompositorOpenGLTest, NoSurfaceIgnored) {
  UseEngineWithView(/*add_surface = */ false);

  auto compositor =
      CompositorOpenGL{engine(), kMockResolver, /*enable_impeller=*/false};

  FlutterBackingStoreConfig config = {};
  FlutterBackingStore backing_store = {};

  EXPECT_CALL(*render_context(), MakeCurrent).WillOnce(Return(true));
  ASSERT_TRUE(compositor.CreateBackingStore(config, &backing_store));

  FlutterLayer layer = {};
  layer.type = kFlutterLayerContentTypeBackingStore;
  layer.backing_store = &backing_store;
  const FlutterLayer* layer_ptr = &layer;

  EXPECT_FALSE(compositor.Present(view(), &layer_ptr, 1));

  ASSERT_TRUE(compositor.CollectBackingStore(&backing_store));
}

// CreateBackingStore picks GL_RGBA16F when the EGL Manager advertises an
// RGBA16Float framebuffer config. The chosen format reaches the engine via
// FlutterBackingStore.open_gl.framebuffer.target, which is the only public
// observable for the compositor's per-platform format selection.
TEST_F(CompositorOpenGLTest, CreateBackingStoreF16) {
  UseHeadlessEngine();

  EXPECT_CALL(*egl_manager(), is_rgba16float)
      .WillRepeatedly(Return(true));

  auto compositor =
      CompositorOpenGL{engine(), kMockResolver, /*enable_impeller=*/false};

  FlutterBackingStoreConfig config = {};
  FlutterBackingStore backing_store = {};

  EXPECT_CALL(*render_context(), MakeCurrent).WillOnce(Return(true));
  ASSERT_TRUE(compositor.CreateBackingStore(config, &backing_store));
  EXPECT_EQ(backing_store.open_gl.framebuffer.target,
            static_cast<uint32_t>(GL_RGBA16F));
  ASSERT_TRUE(compositor.CollectBackingStore(&backing_store));
}

// CreateBackingStore picks GL_BGRA8_EXT when F16 is unavailable and the GLES
// driver exposes GL_EXT_texture_format_BGRA8888. This is ANGLE's normal path
// on Windows so it's the most common case.
TEST_F(CompositorOpenGLTest, CreateBackingStoreBGRA8) {
  UseHeadlessEngine();

  // is_rgba16float() defaults to false via MockManager's ON_CALL.
  // kMockResolver's MockGetStringi returns GL_ANGLE_framebuffer_blit, but
  // the compositor probes for GL_EXT_texture_format_BGRA8888 separately
  // through gl_->GetDescription()->HasExtension. Override the resolver to
  // expose BGRA8888 via GL_EXTENSIONS so the BGRA branch is taken.
  const impeller::ProcTableGLES::Resolver bgra_resolver = [](const char* name) {
    std::string function_name{name};
    if (function_name == "glGetStringi") {
      return reinterpret_cast<void*>(
          +[](GLenum name, int index) -> const unsigned char* {
            if (name == GL_EXTENSIONS) {
              return reinterpret_cast<const unsigned char*>(
                  "GL_EXT_texture_format_BGRA8888");
            }
            return reinterpret_cast<const unsigned char*>("");
          });
    }
    return kMockResolver(name);
  };

  auto compositor =
      CompositorOpenGL{engine(), bgra_resolver, /*enable_impeller=*/false};

  FlutterBackingStoreConfig config = {};
  FlutterBackingStore backing_store = {};

  EXPECT_CALL(*render_context(), MakeCurrent).WillOnce(Return(true));
  ASSERT_TRUE(compositor.CreateBackingStore(config, &backing_store));
  EXPECT_EQ(backing_store.open_gl.framebuffer.target,
            static_cast<uint32_t>(GL_BGRA8_EXT));
  ASSERT_TRUE(compositor.CollectBackingStore(&backing_store));
}

// CreateBackingStore falls back to GL_RGBA8 when neither F16 nor the BGRA
// extension is available. This is the lowest-common-denominator path; ANGLE
// always advertises BGRA8888 on Windows so this should not fire in practice,
// but the upstream embedder may run against GLES drivers that don't.
TEST_F(CompositorOpenGLTest, CreateBackingStoreRGBA8Fallback) {
  UseHeadlessEngine();

  // is_rgba16float() defaults to false. kMockResolver's MockGetStringi
  // returns GL_ANGLE_framebuffer_blit with no BGRA8888 advertised, so the
  // compositor falls through to GL_RGBA8. No resolver override needed.

  auto compositor =
      CompositorOpenGL{engine(), kMockResolver, /*enable_impeller=*/false};

  FlutterBackingStoreConfig config = {};
  FlutterBackingStore backing_store = {};

  EXPECT_CALL(*render_context(), MakeCurrent).WillOnce(Return(true));
  ASSERT_TRUE(compositor.CreateBackingStore(config, &backing_store));
  EXPECT_EQ(backing_store.open_gl.framebuffer.target,
            static_cast<uint32_t>(GL_RGBA8));
  ASSERT_TRUE(compositor.CollectBackingStore(&backing_store));
}

// glTexImage2D is called with internal_format and pixel_type that match the
// chosen TextureFormat. The F16 path requires GL_RGBA16F + GL_HALF_FLOAT
// (sized internal format paired with a float pixel type) — without that pair
// GLES rejects the texture allocation. This test asserts the pairing reaches
// the GL driver rather than just the FlutterBackingStore output, since the
// driver-facing call is where format mismatches manifest as silent zeros or
// allocation failures.
TEST_F(CompositorOpenGLTest, CreateBackingStoreF16TexImage2DArgs) {
  UseHeadlessEngine();

  EXPECT_CALL(*egl_manager(), is_rgba16float)
      .WillRepeatedly(Return(true));

  // Static state because the resolver returns a plain function pointer.
  // Reset at test entry to avoid bleed across runs in the same binary.
  static GLenum captured_internal_format = 0;
  static GLenum captured_format = 0;
  static GLenum captured_type = 0;
  captured_internal_format = 0;
  captured_format = 0;
  captured_type = 0;

  auto MockTexImage2D = +[](GLenum target, GLint level, GLint internal_format,
                             GLsizei width, GLsizei height, GLint border,
                             GLenum format, GLenum type, const void* pixels) {
    captured_internal_format = static_cast<GLenum>(internal_format);
    captured_format = format;
    captured_type = type;
  };

  const impeller::ProcTableGLES::Resolver capturing_resolver =
      [&](const char* name) {
        std::string function_name{name};
        if (function_name == "glTexImage2D") {
          return reinterpret_cast<void*>(MockTexImage2D);
        }
        return kMockResolver(name);
      };

  auto compositor = CompositorOpenGL{engine(), capturing_resolver,
                                     /*enable_impeller=*/false};

  FlutterBackingStoreConfig config = {};
  config.size.width = 128;
  config.size.height = 128;
  FlutterBackingStore backing_store = {};

  EXPECT_CALL(*render_context(), MakeCurrent).WillOnce(Return(true));
  ASSERT_TRUE(compositor.CreateBackingStore(config, &backing_store));

  EXPECT_EQ(captured_internal_format, static_cast<GLenum>(GL_RGBA16F));
  EXPECT_EQ(captured_format, static_cast<GLenum>(GL_RGBA));
  EXPECT_EQ(captured_type, static_cast<GLenum>(GL_HALF_FLOAT));

  ASSERT_TRUE(compositor.CollectBackingStore(&backing_store));
}

TEST_F(CompositorOpenGLTest, PresentUsingANGLEBlitExtension) {
  UseEngineWithView();

  bool resolved_ANGLE_blit = false;
  const impeller::ProcTableGLES::Resolver resolver =
      [&resolved_ANGLE_blit](const char* name) {
        std::string function_name{name};

        if (function_name == "glBlitFramebuffer") {
          return (void*)nullptr;
        } else if (function_name == "glBlitFramebufferANGLE") {
          resolved_ANGLE_blit = true;
          return reinterpret_cast<void*>(&DoNothing);
        }

        return kMockResolver(name);
      };

  auto compositor =
      CompositorOpenGL{engine(), resolver, /*enable_impeller=*/false};

  FlutterBackingStoreConfig config = {};
  FlutterBackingStore backing_store = {};

  EXPECT_CALL(*render_context(), MakeCurrent).WillOnce(Return(true));
  ASSERT_TRUE(compositor.CreateBackingStore(config, &backing_store));

  FlutterLayer layer = {};
  layer.type = kFlutterLayerContentTypeBackingStore;
  layer.backing_store = &backing_store;
  const FlutterLayer* layer_ptr = &layer;

  EXPECT_CALL(*surface(), IsValid).WillRepeatedly(Return(true));
  EXPECT_CALL(*surface(), MakeCurrent).WillOnce(Return(true));
  EXPECT_CALL(*surface(), SwapBuffers).WillOnce(Return(true));
  EXPECT_TRUE(compositor.Present(view(), &layer_ptr, 1));
  EXPECT_TRUE(resolved_ANGLE_blit);

  ASSERT_TRUE(compositor.CollectBackingStore(&backing_store));
}

}  // namespace testing
}  // namespace flutter
