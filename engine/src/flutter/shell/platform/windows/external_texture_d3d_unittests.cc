// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/windows/external_texture_d3d.h"

#include <atomic>
#include <memory>

#include "flutter/shell/platform/windows/egl/proc_table.h"
#include "flutter/shell/platform/windows/testing/egl/mock_manager.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace flutter {
namespace testing {
namespace {

using ::testing::_;
using ::testing::AnyNumber;
using ::testing::DoAll;
using ::testing::Field;
using ::testing::Return;
using ::testing::SaveArg;

// Minimal in-process ID3D11Texture2D fake. Only `GetDesc()` is meaningful;
// the other vtable methods are present to satisfy the interface and never
// driven by ExternalTextureD3d's code path. Fixed at 64x64 RGBA16Float
// unless reformatted via the constructor.
class FakeD3D11Texture2D : public ID3D11Texture2D {
 public:
  explicit FakeD3D11Texture2D(DXGI_FORMAT format) : format_(format) {}

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (ppv == nullptr) {
      return E_POINTER;
    }
    if (IsEqualGUID(riid, __uuidof(IUnknown)) ||
        IsEqualGUID(riid, __uuidof(ID3D11DeviceChild)) ||
        IsEqualGUID(riid, __uuidof(ID3D11Resource)) ||
        IsEqualGUID(riid, __uuidof(ID3D11Texture2D))) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_count_; }
  ULONG STDMETHODCALLTYPE Release() override {
    // Stack-allocated test fixtures never reach refcount 0 via Release alone;
    // do not delete here.
    return --ref_count_;
  }

  // ID3D11DeviceChild
  void STDMETHODCALLTYPE GetDevice(ID3D11Device** ppDevice) override {
    *ppDevice = nullptr;
  }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,
                                            UINT*,
                                            void*) override {
    return E_FAIL;
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,
                                            UINT,
                                            const void*) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  SetPrivateDataInterface(REFGUID, const IUnknown*) override {
    return S_OK;
  }

  // ID3D11Resource
  void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* type) override {
    *type = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
  }
  void STDMETHODCALLTYPE SetEvictionPriority(UINT) override {}
  UINT STDMETHODCALLTYPE GetEvictionPriority() override { return 0; }

  // ID3D11Texture2D
  void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE2D_DESC* desc) override {
    desc->Width = 64;
    desc->Height = 64;
    desc->Format = format_;
    desc->MipLevels = 1;
    desc->ArraySize = 1;
    desc->SampleDesc.Count = 1;
    desc->SampleDesc.Quality = 0;
    desc->Usage = D3D11_USAGE_DEFAULT;
    desc->BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc->CPUAccessFlags = 0;
    desc->MiscFlags = 0;
  }

 private:
  DXGI_FORMAT format_;
  std::atomic<ULONG> ref_count_{1};
};

// Stub ProcTable that no-ops every GL call. ExternalTextureD3d does call
// GenTextures/BindTexture/TexParameteri during PopulateTexture; without a
// stub these would invoke the real ProcTable's nullptr function pointers
// and crash.
class StubProcTable : public flutter::egl::ProcTable {
 public:
  StubProcTable() = default;
  void GenTextures(GLsizei n, GLuint* textures) const override {
    // Hand back any non-zero name so ExternalTextureD3d sees the texture
    // as "already created" on subsequent calls.
    for (GLsizei i = 0; i < n; i++) {
      textures[i] = 1;
    }
  }
  void DeleteTextures(GLsizei, const GLuint*) const override {}
  void BindTexture(GLenum, GLuint) const override {}
  void TexParameteri(GLenum, GLenum, GLint) const override {}
  void TexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum,
                  GLenum, const void*) const override {}
};

// Each test uses this descriptor + a static callback to drive
// PopulateTexture. The callback returns the descriptor pointer through
// `user_data`, sized appropriately.
struct CallbackContext {
  FlutterDesktopGpuSurfaceDescriptor descriptor;
};

const FlutterDesktopGpuSurfaceDescriptor* SurfaceCallback(size_t,
                                                            size_t,
                                                            void* user_data) {
  if (user_data == nullptr) {
    return nullptr;
  }
  return &static_cast<CallbackContext*>(user_data)->descriptor;
}

}  // namespace

// Smoke: when the descriptor callback returns nullptr, PopulateTexture
// returns false without touching any EGL or D3D state. Guards against a
// regression where the early return on null descriptor was removed.
TEST(ExternalTextureD3dTest, NullDescriptorReturnsFalse) {
  egl::MockManager mock_manager;
  EXPECT_CALL(mock_manager, CreateSurfaceFromHandle).Times(0);

  auto gl = std::make_shared<StubProcTable>();
  ExternalTextureD3d external_texture(
      kFlutterDesktopGpuSurfaceTypeD3d11Texture2D,
      [](size_t, size_t, void*) -> const FlutterDesktopGpuSurfaceDescriptor* {
        return nullptr;
      },
      /*user_data=*/nullptr, &mock_manager, gl);

  FlutterOpenGLTexture opengl_texture = {};
  EXPECT_FALSE(external_texture.PopulateTexture(64, 64, &opengl_texture));
}

// kFlutterDesktopGpuSurfaceTypeD3d11Texture2D + RGBA16Float source: the
// format detection should flip is_rgba16float_=true and CreateSurfaceFromHandle
// should be called with EGL_D3D_TEXTURE_ANGLE (direct-pointer) plus
// is_rgba16float=true. The original D3D handle is passed unchanged because
// D3D11Texture2D types don't need the ANGLE-device shared-resource open.
TEST(ExternalTextureD3dTest, D3D11Texture2DFP16RoutesDirectPointerWithFp16) {
  egl::MockManager mock_manager;
  FakeD3D11Texture2D fake_texture(DXGI_FORMAT_R16G16B16A16_FLOAT);

  CallbackContext ctx{};
  ctx.descriptor.struct_size = sizeof(ctx.descriptor);
  ctx.descriptor.handle = static_cast<void*>(&fake_texture);
  ctx.descriptor.width = 64;
  ctx.descriptor.height = 64;
  ctx.descriptor.visible_width = 64;
  ctx.descriptor.visible_height = 64;

  EGLenum captured_handle_type = 0;
  EGLClientBuffer captured_buffer = nullptr;
  bool captured_is_fp16 = false;
  EXPECT_CALL(mock_manager, CreateSurfaceFromHandle)
      .WillOnce(DoAll(SaveArg<0>(&captured_handle_type),
                      SaveArg<1>(&captured_buffer),
                      SaveArg<3>(&captured_is_fp16),
                      Return(EGL_NO_SURFACE)));

  auto gl = std::make_shared<StubProcTable>();
  ExternalTextureD3d external_texture(
      kFlutterDesktopGpuSurfaceTypeD3d11Texture2D, SurfaceCallback, &ctx,
      &mock_manager, gl);

  FlutterOpenGLTexture opengl_texture = {};
  // Returns false because we mocked CreateSurfaceFromHandle to fail —
  // intentional, so we never invoke the eglBindTexImage extern.
  EXPECT_FALSE(external_texture.PopulateTexture(64, 64, &opengl_texture));

  EXPECT_EQ(captured_handle_type,
            static_cast<EGLenum>(EGL_D3D_TEXTURE_ANGLE));
  EXPECT_EQ(captured_buffer, static_cast<EGLClientBuffer>(&fake_texture));
  EXPECT_TRUE(captured_is_fp16);
}

// kFlutterDesktopGpuSurfaceTypeD3d11Texture2D + 8-bit source: same direct-
// pointer path (the type, not the format, picks the binding mode), but
// is_rgba16float should be false.
TEST(ExternalTextureD3dTest, D3D11Texture2DSDRRoutesDirectPointerNoFp16) {
  egl::MockManager mock_manager;
  FakeD3D11Texture2D fake_texture(DXGI_FORMAT_R8G8B8A8_UNORM);

  CallbackContext ctx{};
  ctx.descriptor.struct_size = sizeof(ctx.descriptor);
  ctx.descriptor.handle = static_cast<void*>(&fake_texture);
  ctx.descriptor.width = 64;
  ctx.descriptor.height = 64;
  ctx.descriptor.visible_width = 64;
  ctx.descriptor.visible_height = 64;

  EGLenum captured_handle_type = 0;
  bool captured_is_fp16 = true;  // start true; expect mock to leave false
  EXPECT_CALL(mock_manager, CreateSurfaceFromHandle)
      .WillOnce(DoAll(SaveArg<0>(&captured_handle_type),
                      SaveArg<3>(&captured_is_fp16),
                      Return(EGL_NO_SURFACE)));

  auto gl = std::make_shared<StubProcTable>();
  ExternalTextureD3d external_texture(
      kFlutterDesktopGpuSurfaceTypeD3d11Texture2D, SurfaceCallback, &ctx,
      &mock_manager, gl);

  FlutterOpenGLTexture opengl_texture = {};
  EXPECT_FALSE(external_texture.PopulateTexture(64, 64, &opengl_texture));

  EXPECT_EQ(captured_handle_type,
            static_cast<EGLenum>(EGL_D3D_TEXTURE_ANGLE));
  EXPECT_FALSE(captured_is_fp16);
}

// kFlutterDesktopGpuSurfaceTypeDxgiSharedHandle when ANGLE has no usable
// device: GetDevice returns false, the code falls through to the legacy
// pbuffer-from-share-handle path. CreateSurfaceFromHandle should be called
// with EGL_D3D_TEXTURE_2D_SHARE_HANDLE_ANGLE (not EGL_D3D_TEXTURE_ANGLE),
// the original raw handle, and is_rgba16float=false.
TEST(ExternalTextureD3dTest, DxgiSharedHandleNoAngleDeviceUsesShareHandlePath) {
  egl::MockManager mock_manager;

  // No ANGLE device available — the FP16 detection branch is skipped.
  EXPECT_CALL(mock_manager, GetDevice(_)).WillOnce(Return(false));

  // Use a non-null sentinel for the raw shared handle. The descriptor's
  // `handle` field is a void* and ExternalTextureD3d only dereferences it
  // when type == kFlutterDesktopGpuSurfaceTypeD3d11Texture2D.
  void* raw_handle = reinterpret_cast<void*>(0xDEADBEEF);

  CallbackContext ctx{};
  ctx.descriptor.struct_size = sizeof(ctx.descriptor);
  ctx.descriptor.handle = raw_handle;
  ctx.descriptor.width = 64;
  ctx.descriptor.height = 64;
  ctx.descriptor.visible_width = 64;
  ctx.descriptor.visible_height = 64;

  EGLenum captured_handle_type = 0;
  EGLClientBuffer captured_buffer = nullptr;
  bool captured_is_fp16 = true;
  EXPECT_CALL(mock_manager, CreateSurfaceFromHandle)
      .WillOnce(DoAll(SaveArg<0>(&captured_handle_type),
                      SaveArg<1>(&captured_buffer),
                      SaveArg<3>(&captured_is_fp16),
                      Return(EGL_NO_SURFACE)));

  auto gl = std::make_shared<StubProcTable>();
  ExternalTextureD3d external_texture(
      kFlutterDesktopGpuSurfaceTypeDxgiSharedHandle, SurfaceCallback, &ctx,
      &mock_manager, gl);

  FlutterOpenGLTexture opengl_texture = {};
  EXPECT_FALSE(external_texture.PopulateTexture(64, 64, &opengl_texture));

  EXPECT_EQ(captured_handle_type,
            static_cast<EGLenum>(EGL_D3D_TEXTURE_2D_SHARE_HANDLE_ANGLE));
  EXPECT_EQ(captured_buffer, static_cast<EGLClientBuffer>(raw_handle));
  EXPECT_FALSE(captured_is_fp16);
}

}  // namespace testing
}  // namespace flutter
