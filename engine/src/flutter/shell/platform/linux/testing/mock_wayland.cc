// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Link-time stubs for the libwayland-client symbols referenced by
// FlCompositorHDR and the generated protocol code. The production library
// leaves these undefined (linked with -z,undefs; they resolve at runtime
// from the libwayland-client GTK already loads), but the unittests
// EXECUTABLE must fully link — and the interface structs are data symbols,
// which the loader resolves eagerly even when unused.
//
// FlCompositorHDR never constructs in the test environment (its factory
// requires a live Wayland session), so nothing here executes: these are
// symbol placeholders, and the natural seed for a future behavioral
// mock-wayland harness.

#include <cstdint>

extern "C" {

// Layout-compatible with libwayland's struct wl_interface; only the symbol
// (its address) is ever used by the linked code paths tests can reach.
struct MockWlInterface {
  const char* name;
  int version;
  int method_count;
  const void* methods;
  int event_count;
  const void* events;
};

// `extern` twice: C++ gives namespace-scope const objects internal linkage
// by default, and the symbol must be visible to the linker.
#define MOCK_WL_INTERFACE(sym)      \
  extern const MockWlInterface sym; \
  const MockWlInterface sym = {#sym, 0, 0, nullptr, 0, nullptr}

MOCK_WL_INTERFACE(wl_buffer_interface);
MOCK_WL_INTERFACE(wl_compositor_interface);
MOCK_WL_INTERFACE(wl_output_interface);
MOCK_WL_INTERFACE(wl_region_interface);
MOCK_WL_INTERFACE(wl_registry_interface);
MOCK_WL_INTERFACE(wl_subcompositor_interface);
MOCK_WL_INTERFACE(wl_subsurface_interface);
MOCK_WL_INTERFACE(wl_surface_interface);

void* wl_display_create_queue(void* display) {
  return nullptr;
}

int wl_display_dispatch_queue_pending(void* display, void* queue) {
  return 0;
}

int wl_display_flush(void* display) {
  return 0;
}

int wl_display_roundtrip_queue(void* display, void* queue) {
  return 0;
}

void wl_event_queue_destroy(void* queue) {}

int wl_proxy_add_listener(void* proxy,
                          void (**implementation)(void),
                          void* data) {
  return 0;
}

void wl_proxy_destroy(void* proxy) {}

uint32_t wl_proxy_get_version(void* proxy) {
  return 0;
}

void* wl_proxy_marshal_flags(void* proxy,
                             uint32_t opcode,
                             const void* interface,
                             uint32_t version,
                             uint32_t flags,
                             ...) {
  return nullptr;
}

void wl_proxy_set_queue(void* proxy, void* queue) {}

}  // extern "C"
