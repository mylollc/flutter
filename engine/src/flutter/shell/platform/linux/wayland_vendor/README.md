# Vendored Wayland client headers + protocol stubs

Used by the HDR presentation path (`fl_compositor_hdr.cc`), which speaks the
linux-dmabuf, viewporter and color-management Wayland protocols directly.

Two kinds of files, both vendored because the pinned Debian sysroot's wayland
(1.18) predates what the HDR path needs (`wl_proxy_marshal_flags` arrived in
wayland 1.20):

1. **libwayland-client public headers** (`wayland-client.h`,
   `wayland-client-core.h`, `wayland-client-protocol.h`, `wayland-util.h`,
   `wayland-version.h`): copied verbatim from the wayland **1.20.0** release
   (`wayland-client-protocol.h` generated from that release's `wayland.xml`
   with wayland-scanner). Headers only — no wayland library is vendored or
   linked: the referenced symbols stay undefined in `libflutter_linux_gtk.so`
   (see the `allow_undefined_symbols` config in `../BUILD.gn`) and resolve at
   runtime from the system `libwayland-client.so.0` that GTK already loads.
   Wayland >= 1.20 is therefore a RUNTIME requirement of the HDR path only —
   sessions with older wayland (or none of the required globals) fall back to
   the stock compositor, and the supported-platform floor is unchanged.

2. **Generated protocol bindings** (`*-client-protocol.h` + `*-protocol.c`
   for `linux-dmabuf-v1`, `viewporter`, `color-management-v1`): the output of
   `wayland-scanner client-header` / `wayland-scanner private-code` over the
   corresponding XML in the wayland-protocols repository
   (https://gitlab.freedesktop.org/wayland/wayland-protocols) — linux-dmabuf
   and viewporter from the stable set, color-management from
   staging/color-management (v1). Regenerate with:

       wayland-scanner client-header <proto>.xml <proto>-client-protocol.h
       wayland-scanner private-code  <proto>.xml <proto>-protocol.c

All files carry their upstream MIT copyright notices.
