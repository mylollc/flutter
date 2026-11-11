// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/windows/compositor_opengl.h"

#include <windows.h>

#include <vector>

#include "GLES3/gl3.h"
#include "flutter/shell/platform/windows/flutter_windows_engine.h"
#include "flutter/shell/platform/windows/flutter_windows_view.h"

namespace flutter {

namespace {

constexpr uint32_t kWindowFrameBufferId = 0;

// The metadata for an OpenGL framebuffer backing store.
struct FramebufferBackingStore {
  uint32_t framebuffer_id;
  uint32_t texture_id;
};

typedef const impeller::GLProc<decltype(glBlitFramebuffer)> BlitFramebufferProc;

const BlitFramebufferProc& GetBlitFramebufferProc(
    const impeller::ProcTableGLES& gl) {
  if (gl.BlitFramebuffer.IsAvailable()) {
    return gl.BlitFramebuffer;
  } else if (gl.BlitFramebufferANGLE.IsAvailable()) {
    return gl.BlitFramebufferANGLE;
  }

  // CompositorOpenGL::Initialize verifies that a blit procedure is available.
  FML_UNREACHABLE();
}

// The F16 present pass (see |CompositorOpenGL::present_program_|). GLSL ES
// 1.00: the window's context is GLES 2. One triangle covers the viewport; the
// backing store and the window surface have the same size and the same
// bottom-left origin, so the texture coordinate is the clip position mapped
// to [0, 1].
constexpr char kPresentVertexShader[] = R"(
attribute vec2 aPosition;
varying vec2 vTexCoord;
void main() {
  vTexCoord = aPosition * 0.5 + 0.5;
  gl_Position = vec4(aPosition, 0.0, 1.0);
}
)";

// Skia stores premultiplied, sRGB-encoded values. Un-premultiply, apply the
// sRGB EOTF (mirrored for negative values and continued above 1.0, as Skia's
// extended sRGB does), scale to scRGB's 80-nit unit, premultiply again.
constexpr char kPresentFragmentShader[] = R"(
precision highp float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
uniform float uScale;
vec3 SrgbToLinear(vec3 c) {
  vec3 a = abs(c);
  vec3 lo = a / 12.92;
  vec3 hi = pow((a + 0.055) / 1.055, vec3(2.4));
  return sign(c) * mix(hi, lo, step(a, vec3(0.04045)));
}
void main() {
  vec4 c = texture2D(uTexture, vTexCoord);
  vec3 rgb = c.a > 0.0 ? c.rgb / c.a : vec3(0.0);
  gl_FragColor = vec4(SrgbToLinear(rgb) * uScale * c.a, c.a);
}
)";

}  // namespace

CompositorOpenGL::CompositorOpenGL(FlutterWindowsEngine* engine,
                                   impeller::ProcTableGLES::Resolver resolver,
                                   bool enable_impeller)
    : engine_(engine), resolver_(resolver), enable_impeller_(enable_impeller) {}

bool CompositorOpenGL::CreateBackingStore(
    const FlutterBackingStoreConfig& config,
    FlutterBackingStore* result) {
  if (!is_initialized_ && !Initialize()) {
    return false;
  }

  auto store = std::make_unique<FramebufferBackingStore>();

  gl_->GenTextures(1, &store->texture_id);
  gl_->GenFramebuffers(1, &store->framebuffer_id);

  gl_->BindFramebuffer(GL_FRAMEBUFFER, store->framebuffer_id);

  gl_->BindTexture(GL_TEXTURE_2D, store->texture_id);
  gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  gl_->TexImage2D(GL_TEXTURE_2D, 0, format_.internal_format, config.size.width,
                  config.size.height, 0, format_.general_format,
                  format_.pixel_type, nullptr);
  gl_->BindTexture(GL_TEXTURE_2D, 0);

  if (enable_impeller_) {
    // Impeller requries that its onscreen surface is Multisampled and already
    // has depth/stencil attached in order for anti-aliasing to work.
    gl_->FramebufferTexture2DMultisampleEXT(GL_FRAMEBUFFER,
                                            GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                            store->texture_id, 0, 4);

    // Set up depth/stencil attachment for impeller renderer.
    GLuint depth_stencil;
    gl_->GenRenderbuffers(1, &depth_stencil);
    gl_->BindRenderbuffer(GL_RENDERBUFFER, depth_stencil);
    gl_->RenderbufferStorageMultisampleEXT(
        GL_RENDERBUFFER,      // target
        4,                    // samples
        GL_DEPTH24_STENCIL8,  // internal format
        config.size.width,    // width
        config.size.height    // height
    );
    gl_->FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                 GL_RENDERBUFFER, depth_stencil);
    gl_->FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                                 GL_RENDERBUFFER, depth_stencil);

  } else {
    gl_->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_TEXTURE_2D, store->texture_id, 0);
  }

  result->type = kFlutterBackingStoreTypeOpenGL;
  result->open_gl.type = kFlutterOpenGLTargetTypeFramebuffer;
  result->open_gl.framebuffer.name = store->framebuffer_id;
  result->open_gl.framebuffer.target = format_.sized_format;
  result->open_gl.framebuffer.user_data = store.release();
  result->open_gl.framebuffer.destruction_callback = [](void* user_data) {
    // Backing store destroyed in `CompositorOpenGL::CollectBackingStore`, set
    // on FlutterCompositor.collect_backing_store_callback during engine start.
  };
  return true;
}

bool CompositorOpenGL::CollectBackingStore(const FlutterBackingStore* store) {
  FML_DCHECK(is_initialized_);
  FML_DCHECK(store->type == kFlutterBackingStoreTypeOpenGL);
  FML_DCHECK(store->open_gl.type == kFlutterOpenGLTargetTypeFramebuffer);

  auto user_data = static_cast<FramebufferBackingStore*>(
      store->open_gl.framebuffer.user_data);

  gl_->DeleteFramebuffers(1, &user_data->framebuffer_id);
  gl_->DeleteTextures(1, &user_data->texture_id);

  delete user_data;
  return true;
}

bool CompositorOpenGL::Present(FlutterWindowsView* view,
                               const FlutterLayer** layers,
                               size_t layers_count) {
  FML_DCHECK(view != nullptr);

  // Clear the view if there are no layers to present.
  if (layers_count == 0) {
    // Normally the compositor is initialized when the first backing store is
    // created. However, on an empty frame no backing stores are created and
    // the present needs to initialize the compositor.
    if (!is_initialized_ && !Initialize()) {
      return false;
    }

    return Clear(view);
  }

  // TODO: Support compositing layers and platform views.
  // See: https://github.com/flutter/flutter/issues/31713
  FML_DCHECK(is_initialized_);
  FML_DCHECK(layers_count == 1);
  FML_DCHECK(layers[0]->offset.x == 0 && layers[0]->offset.y == 0);
  FML_DCHECK(layers[0]->type == kFlutterLayerContentTypeBackingStore);
  FML_DCHECK(layers[0]->backing_store->type == kFlutterBackingStoreTypeOpenGL);
  FML_DCHECK(layers[0]->backing_store->open_gl.type ==
             kFlutterOpenGLTargetTypeFramebuffer);

  auto width = layers[0]->size.width;
  auto height = layers[0]->size.height;

  // Check if this frame can be presented. This resizes the surface if a resize
  // is pending and |width| and |height| match the target size.
  if (!view->OnFrameGenerated(width, height)) {
    return false;
  }

  // |OnFrameGenerated| should return false if the surface isn't valid.
  FML_DCHECK(view->surface() != nullptr);
  FML_DCHECK(view->surface()->IsValid());

  egl::WindowSurface* surface = view->surface();
  if (!surface->MakeCurrent()) {
    return false;
  }

  auto source_id = layers[0]->backing_store->open_gl.framebuffer.name;

  // Disable the scissor test as it can affect blit operations.
  // Prevents regressions like: https://github.com/flutter/flutter/issues/140828
  // See OpenGL specification version 4.6, section 18.3.1.
  gl_->Disable(GL_SCISSOR_TEST);

  // F16: convert the sRGB-composited backing store to scRGB in one draw
  // instead of copying it (see |present_program_|). The engine resets Skia's
  // GL state before it renders the next frame, as it does after the blit.
  if (format_.pixel_type == GL_HALF_FLOAT && EnsurePresentProgram()) {
    auto store = static_cast<FramebufferBackingStore*>(
        layers[0]->backing_store->open_gl.framebuffer.user_data);

    // Skia caches GL state in this context and re-reads it only when the
    // engine resets it, which happens after the next frame's raster-cache and
    // snapshot rendering. Anything this draw changes that Skia tracks (blend,
    // program, texture, vertex attribute 0, viewport...) is put back
    // afterwards, or those offscreen draws run with the wrong state (glyphs
    // and cached layers come out as solid rectangles without blending).
    GLint saved_framebuffer = 0, saved_program = 0, saved_active_texture = 0,
          saved_texture = 0, saved_array_buffer = 0;
    GLint saved_viewport[4] = {};
    GLboolean saved_blend = GL_FALSE, saved_depth = GL_FALSE,
              saved_stencil = GL_FALSE, saved_cull = GL_FALSE;
    GLboolean saved_color_mask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLint attrib_enabled = 0, attrib_buffer = 0, attrib_size = 4,
          attrib_type = GL_FLOAT, attrib_normalized = 0, attrib_stride = 0;
    void* attrib_pointer = nullptr;
    gl_->GetIntegerv(GL_FRAMEBUFFER_BINDING, &saved_framebuffer);
    gl_->GetIntegerv(GL_CURRENT_PROGRAM, &saved_program);
    gl_->GetIntegerv(GL_ACTIVE_TEXTURE, &saved_active_texture);
    gl_->ActiveTexture(GL_TEXTURE0);
    gl_->GetIntegerv(GL_TEXTURE_BINDING_2D, &saved_texture);
    gl_->GetIntegerv(GL_ARRAY_BUFFER_BINDING, &saved_array_buffer);
    gl_->GetIntegerv(GL_VIEWPORT, saved_viewport);
    gl_->GetBooleanv(GL_BLEND, &saved_blend);
    gl_->GetBooleanv(GL_DEPTH_TEST, &saved_depth);
    gl_->GetBooleanv(GL_STENCIL_TEST, &saved_stencil);
    gl_->GetBooleanv(GL_CULL_FACE, &saved_cull);
    gl_->GetBooleanv(GL_COLOR_WRITEMASK, saved_color_mask);
    get_vertex_attrib_iv_(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attrib_enabled);
    get_vertex_attrib_iv_(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,
                          &attrib_buffer);
    get_vertex_attrib_iv_(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &attrib_size);
    get_vertex_attrib_iv_(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &attrib_type);
    get_vertex_attrib_iv_(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,
                          &attrib_normalized);
    get_vertex_attrib_iv_(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &attrib_stride);
    get_vertex_attrib_pointer_v_(0, GL_VERTEX_ATTRIB_ARRAY_POINTER,
                                 &attrib_pointer);

    gl_->BindFramebuffer(GL_FRAMEBUFFER, kWindowFrameBufferId);
    gl_->Viewport(0, 0, width, height);
    gl_->Disable(GL_BLEND);
    gl_->Disable(GL_DEPTH_TEST);
    gl_->Disable(GL_STENCIL_TEST);
    gl_->Disable(GL_CULL_FACE);
    gl_->ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl_->UseProgram(present_program_);
    gl_->BindTexture(GL_TEXTURE_2D, store->texture_id);
    gl_->Uniform1i(present_texture_location_, 0);
    const float scale = SdrWhiteScale(view->GetWindowHandle());
    gl_->Uniform1fv(present_scale_location_, 1, &scale);
    gl_->BindBuffer(GL_ARRAY_BUFFER, present_vertex_buffer_);
    gl_->VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    gl_->EnableVertexAttribArray(0);
    gl_->DrawArrays(GL_TRIANGLES, 0, 3);

    gl_->BindBuffer(GL_ARRAY_BUFFER, attrib_buffer);
    gl_->VertexAttribPointer(0, attrib_size, attrib_type,
                             attrib_normalized ? GL_TRUE : GL_FALSE,
                             attrib_stride, attrib_pointer);
    if (attrib_enabled) {
      gl_->EnableVertexAttribArray(0);
    } else {
      gl_->DisableVertexAttribArray(0);
    }
    gl_->BindBuffer(GL_ARRAY_BUFFER, saved_array_buffer);
    gl_->BindTexture(GL_TEXTURE_2D, saved_texture);
    gl_->ActiveTexture(saved_active_texture);
    gl_->UseProgram(saved_program);
    gl_->Viewport(saved_viewport[0], saved_viewport[1], saved_viewport[2],
                  saved_viewport[3]);
    auto restore = [this](GLenum capability, GLboolean enabled) {
      if (enabled) {
        gl_->Enable(capability);
      } else {
        gl_->Disable(capability);
      }
    };
    restore(GL_BLEND, saved_blend);
    restore(GL_DEPTH_TEST, saved_depth);
    restore(GL_STENCIL_TEST, saved_stencil);
    restore(GL_CULL_FACE, saved_cull);
    gl_->ColorMask(saved_color_mask[0], saved_color_mask[1],
                   saved_color_mask[2], saved_color_mask[3]);
    gl_->BindFramebuffer(GL_FRAMEBUFFER, saved_framebuffer);

    if (!surface->SwapBuffers()) {
      return false;
    }
    view->OnFramePresented();
    return true;
  }

  gl_->BindFramebuffer(GL_READ_FRAMEBUFFER, source_id);
  gl_->BindFramebuffer(GL_DRAW_FRAMEBUFFER, kWindowFrameBufferId);

  auto blitFramebuffer = GetBlitFramebufferProc(*gl_);
  blitFramebuffer(0,                    // srcX0
                  0,                    // srcY0
                  width,                // srcX1
                  height,               // srcY1
                  0,                    // dstX0
                  0,                    // dstY0
                  width,                // dstX1
                  height,               // dstY1
                  GL_COLOR_BUFFER_BIT,  // mask
                  GL_NEAREST            // filter
  );

  if (!surface->SwapBuffers()) {
    return false;
  }

  view->OnFramePresented();
  return true;
}

bool CompositorOpenGL::EnsurePresentProgram() {
  if (present_program_ != 0) {
    return true;
  }
  if (present_program_failed_) {
    return false;
  }
  get_vertex_attrib_iv_ =
      reinterpret_cast<decltype(get_vertex_attrib_iv_)>(
          resolver_("glGetVertexAttribiv"));
  get_vertex_attrib_pointer_v_ =
      reinterpret_cast<decltype(get_vertex_attrib_pointer_v_)>(
          resolver_("glGetVertexAttribPointerv"));
  if (get_vertex_attrib_iv_ == nullptr ||
      get_vertex_attrib_pointer_v_ == nullptr) {
    FML_LOG(ERROR) << "F16 surface: glGetVertexAttrib* unavailable; "
                      "presenting with a plain copy.";
    present_program_failed_ = true;
    return false;
  }
  auto compile = [this](GLenum type, const char* source) -> GLuint {
    GLuint shader = gl_->CreateShader(type);
    gl_->ShaderSource(shader, 1, &source, nullptr);
    gl_->CompileShader(shader);
    GLint ok = GL_FALSE;
    gl_->GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
      GLint length = 0;
      gl_->GetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
      std::vector<char> log(length > 0 ? length : 1, '\0');
      gl_->GetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), nullptr,
                            log.data());
      FML_LOG(ERROR) << "F16 present shader did not compile: " << log.data();
      gl_->DeleteShader(shader);
      return 0;
    }
    return shader;
  };
  GLuint vertex = compile(GL_VERTEX_SHADER, kPresentVertexShader);
  GLuint fragment = compile(GL_FRAGMENT_SHADER, kPresentFragmentShader);
  GLuint program = 0;
  if (vertex != 0 && fragment != 0) {
    program = gl_->CreateProgram();
    gl_->AttachShader(program, vertex);
    gl_->AttachShader(program, fragment);
    gl_->BindAttribLocation(program, 0, "aPosition");
    gl_->LinkProgram(program);
    GLint ok = GL_FALSE;
    gl_->GetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
      FML_LOG(ERROR) << "F16 present program did not link.";
      gl_->DeleteProgram(program);
      program = 0;
    }
  }
  if (vertex != 0) {
    gl_->DeleteShader(vertex);
  }
  if (fragment != 0) {
    gl_->DeleteShader(fragment);
  }
  if (program == 0) {
    FML_LOG(ERROR) << "F16 surface: presenting with a plain copy; the UI is "
                      "shown in linear light at 80 nits.";
    present_program_failed_ = true;
    return false;
  }

  // One triangle that covers clip space [-1, 1]^2.
  const GLfloat vertices[] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};
  gl_->GenBuffers(1, &present_vertex_buffer_);
  gl_->BindBuffer(GL_ARRAY_BUFFER, present_vertex_buffer_);
  gl_->BufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices,
                  GL_STATIC_DRAW);
  gl_->BindBuffer(GL_ARRAY_BUFFER, 0);
  present_scale_location_ = gl_->GetUniformLocation(program, "uScale");
  present_texture_location_ = gl_->GetUniformLocation(program, "uTexture");
  present_program_ = program;
  FML_LOG(INFO) << "F16 surface: compositing the UI in sRGB, converting to "
                   "scRGB at present.";
  return true;
}

float CompositorOpenGL::SdrWhiteScale(HWND hwnd) {
  HMONITOR monitor = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  const uint64_t now = ::GetTickCount64();
  if (monitor == sdr_white_monitor_ && now - sdr_white_read_ms_ < 1000) {
    return sdr_white_scale_;
  }
  sdr_white_monitor_ = monitor;
  sdr_white_read_ms_ = now;

  float scale = 1.0f;
  MONITORINFOEXW monitor_info = {};
  monitor_info.cbSize = sizeof(monitor_info);
  UINT32 path_count = 0;
  UINT32 mode_count = 0;
  if (monitor && ::GetMonitorInfoW(monitor, &monitor_info) &&
      ::GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count,
                                    &mode_count) == ERROR_SUCCESS) {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    if (::QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(),
                             &mode_count, modes.data(),
                             nullptr) == ERROR_SUCCESS) {
      for (UINT32 i = 0; i < path_count; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[i].sourceInfo.adapterId;
        source.header.id = paths[i].sourceInfo.id;
        if (::DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
            ::wcscmp(source.viewGdiDeviceName, monitor_info.szDevice) != 0) {
          continue;
        }
        DISPLAYCONFIG_SDR_WHITE_LEVEL white = {};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = paths[i].targetInfo.adapterId;
        white.header.id = paths[i].targetInfo.id;
        // SDRWhiteLevel is in thousandths of 80 nits: 1000 = 80 nits = scRGB
        // 1.0, so the scale is the level / 1000.
        if (::DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS &&
            white.SDRWhiteLevel >= 1000) {
          scale = static_cast<float>(white.SDRWhiteLevel) / 1000.0f;
        }
        break;
      }
    }
  }
  if (scale != sdr_white_scale_) {
    FML_LOG(INFO) << "F16 surface: SDR white " << scale * 80.0f
                  << " nits (scale " << scale << ").";
  }
  sdr_white_scale_ = scale;
  return scale;
}

bool CompositorOpenGL::Initialize() {
  FML_DCHECK(!is_initialized_);

  egl::Manager* manager = engine_->egl_manager();
  if (!manager) {
    return false;
  }

  if (!manager->render_context()->MakeCurrent()) {
    return false;
  }

  gl_ = std::make_unique<impeller::ProcTableGLES>(resolver_);
  if (!gl_->IsValid()) {
    gl_.reset();
    return false;
  }

  // Match the backing-store format to the EGL config: F16 when ANGLE supplied
  // a RGBA16 float-component config, otherwise 8-bit. Keeping the backing
  // store format in sync with the window surface ensures the blit at
  // presentation time doesn't round-trip HDR values through an 8-bit target.
  if (manager->is_rgba16float()) {
    format_.sized_format = GL_RGBA16F;
    format_.internal_format = GL_RGBA16F;
    format_.general_format = GL_RGBA;
    format_.pixel_type = GL_HALF_FLOAT;
  } else if (gl_->GetDescription()->HasExtension(
                 "GL_EXT_texture_format_BGRA8888")) {
    format_.sized_format = GL_BGRA8_EXT;
    format_.internal_format = GL_BGRA_EXT;
    format_.general_format = GL_BGRA_EXT;
    format_.pixel_type = GL_UNSIGNED_BYTE;
  } else {
    format_.sized_format = GL_RGBA8;
    format_.internal_format = GL_RGBA;
    format_.general_format = GL_RGBA;
    format_.pixel_type = GL_UNSIGNED_BYTE;
  }

  if (!gl_->BlitFramebuffer.IsAvailable() &&
      !gl_->BlitFramebufferANGLE.IsAvailable()) {
    FML_LOG(ERROR) << "Unable to find OpenGL blit framebuffer procedure.";
    return false;
  }

  is_initialized_ = true;
  return true;
}

bool CompositorOpenGL::Clear(FlutterWindowsView* view) {
  FML_DCHECK(is_initialized_);

  // Check if this frame can be presented. This resizes the surface if needed.
  if (!view->OnEmptyFrameGenerated()) {
    return false;
  }

  // |OnEmptyFrameGenerated| should return false if the surface isn't valid.
  FML_DCHECK(view->surface() != nullptr);
  FML_DCHECK(view->surface()->IsValid());

  egl::WindowSurface* surface = view->surface();
  if (!surface->MakeCurrent()) {
    return false;
  }

  gl_->ClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  gl_->Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

  if (!surface->SwapBuffers()) {
    return false;
  }

  view->OnFramePresented();
  return true;
}

}  // namespace flutter
