# Upstream candidates carried on this branch

This branch (`OlymEDRSupport`) is Flutter 3.41.2 plus a set of engine and
tool patches. Each entry below is a fix on this branch that upstream
flutter/flutter did not have when last checked, written up so an issue and a
PR can be filed later without re-deriving the case. Nothing here has been
filed; filing an issue without the fix invites an alternate upstream fix that
conflicts with the commit carried here, so an issue is filed only together
with its PR.

Each entry: the commit on this branch (by subject, since the branch is
rebased), what upstream has, the proposed issue text, and what the PR needs
beyond the commit.

Checked against flutter/flutter master on 2026-10-13.

---

## 1. Tonic: an isolate started paused can miss the debugger's resume

**Commit:** `[tonic] Resume a paused-on-start isolate whose resume was taken outside OnHandleMessage`
**File:** `engine/src/flutter/third_party/tonic/dart_message_handler.cc`
**Upstream:** unfixed; the handler is unchanged since flutter/engine#161824 (2025-01).

**Issue text.** With `--pause-isolates-on-start` (every `flutter run
--start-paused` and IDE debug launch), `DartMessageHandler::OnHandleMessage`
leaves paused-on-start only when `Dart_HasServiceMessages()` is true. The VM
also handles service messages whenever it runs Dart code, so if the embedder
calls into Dart before that handler runs (window metrics, a platform message)
the debugger's resume is taken there. Its message is then gone, the next
`OnHandleMessage` sees no service messages and returns, and nothing else
resumes the isolate: `main()` never runs. An embedder that creates a second
engine for a secondary window, with its root isolate started paused, hits this
about one open in five on Linux. The VM's resume handler clears the isolate's
should-pause-on-start flag (runtime/vm/service.cc, `Resume`), so a
paused-on-start isolate whose `Dart_ShouldPauseOnStart()` is false has a
pending resume; treating that as the resume fixes it.

**PR needs:** the commit as is. It carries a regression test,
`DartIsolateTest.PausedOnStartIsolateResumesWhenTheResumeIsTakenOutsideItsMessageHandler`
in `runtime/dart_isolate_unittests.cc`: it starts an isolate paused, sends the
service protocol's `resume` from another thread while the test is inside a
`Dart_Invoke`, and asserts the entrypoint still runs. It fails by timeout on
the unfixed handler.

## 2. flutter_tools: `devicectl list devices` deadlocks when its output exceeds the pipe buffer

**Commit:** `[flutter_tools] Drain devicectl's output before waiting for it to exit`
**File:** `packages/flutter_tools/lib/src/ios/core_devices.dart`
**Upstream:** unfixed on master (same code after the 2026-09-11 reformat, c4f03861bf).

**Issue text.** `IOSCoreDeviceControl._listCoreDevices` starts
`xcrun devicectl list devices --timeout 5 --json-output <file>` and awaits
`process.exitCode` before reading stdout or stderr. Once devicectl writes more
than the pipe buffer it blocks in `write()` and never exits; `--timeout`
cannot fire because the process is blocked on a write, not on device I/O.
`flutter devices` then hangs (observed ~900 s) or returns an empty CoreDevice
list, and `flutter run` falls back to the legacy ios-deploy path, which needs
the `DeveloperDiskImage.dmg` that Xcode 27 no longer ships, so the run fails
with "Could not run … .app". Seen with Xcode 27.1, a physical iPad on iPadOS
26.7, Flutter 3.41.2. Killing the hung `devicectl list devices` children makes
a stuck `flutter run` advance within seconds. Starting the two stream drains
before awaiting the exit code fixes it: `flutter devices` 6 s instead of a
hang, install and launch 39 s instead of 137 s and three failures.

**PR needs:** the commit as is. It carries a regression test in
`core_devices_test.dart` ("drains devicectl output before waiting for it to
exit") whose fake process exits only once both output pipes have a listener;
it times out on the unfixed tool.

## 3. macOS: use-after-free when unregistering an external texture

**Commit:** `[macOS] Fix use-after-free when unregistering an external texture`
**Files:** `FlutterEngine.mm`, `FlutterEngine_Internal.h`, `FlutterTextureRegistrar.mm`, `FlutterEngineTest.mm`
**Upstream:** unfixed; `FlutterTextureRegistrar` still frees the wrapper synchronously and `_textures` is unguarded.

**Issue text.** Unregistering an external texture frees its
`FlutterExternalTexture` wrapper synchronously on the platform thread, while
the raster thread's `EmbedderExternalTextureMetal::ResolveTexture` can still
hold a raw pointer into the wrapper's texture-handle vector for an
already-submitted frame. Under rapid texture churn the wrapper is freed under
an in-flight resolve, which then messages a dangling Metal texture and crashes
with an unrecognized selector on io.flutter.raster. `_textures` is also an
`NSMutableDictionary` read on the raster thread and mutated on the platform
thread. Fix: defer the wrapper's release onto the raster task runner (FIFO
after the registry removal and any queued resolve) and guard the dictionary.

**PR needs:** the commit, which already carries `FlutterEngineTest` coverage.

## 4. Linux: GL teardown aborts when the context is already gone at shutdown

**Commit:** `[Linux] Guard GL resource teardown against a lost context on shutdown`
**Files:** `fl_compositor_opengl.cc`, `fl_framebuffer.cc`, tests, `mock_epoxy`
**Upstream:** unfixed; `fl_framebuffer_dispose` still issues `glDelete*` unconditionally.

**Issue text.** On quit, GApplication shutdown tears down GDK and the
Wayland display before FlView/engine dispose runs. The EGLDisplay comes from
`gdk_wayland_display_get_wl_display()`, so the render context can no longer
be made current; the compositor then disposes its `FlFramebuffer`, whose
dispose calls `glDelete*` with no current context, and epoxy aborts with
"Couldn't find current GLX or EGL context". Users see a window that will not
close and a force-quit dialog. Fix: only delete GL objects when
`eglGetCurrentContext() != EGL_NO_CONTEXT`, and make the compositor dispose
the framebuffer while the context is still current.

**PR needs:** the commit, which carries `FlFramebufferTest` coverage.

## 5. Linux: zero-delta discrete scroll events abort debug builds

**Commit:** `[linux] Drop zero-delta discrete scroll events`
**Files:** `fl_scrolling_manager.cc`, test
**Upstream:** unfixed; the discrete branch still forwards unconditionally.

**Issue text.** GDK emits zero-delta smooth-scroll boundary events on some
mice. `fl_scrolling_manager` forwards every discrete scroll through
`fl_engine_send_mouse_pointer_event` with an arbitrary `kMove` phase, relying
on a non-zero delta to turn it into a scroll signal. A zero-delta event
becomes a `kMove` with no signal and no button on a pointer that was never
down, which `PointerDataPacketConverter` rejects with `FML_DCHECK(state.is_down)`,
aborting debug builds. Fix: forward only when a delta is non-zero.

**PR needs:** the commit, which carries a regression test.

## 6. Windows: no fling after a trackpad swipe (DirectManipulation inertia)

**Commit:** `Windows: forward DirectManipulation inertia frames as PanZoomUpdate`
**Files:** `direct_manipulation.cc`, `.h`, unittests
**Upstream:** unfixed; `during_inertia_` gate and `OnScrollInertiaCancel` still present; flutter/flutter#126649 is open.

**Issue text.** DirectManipulation polls at 60 Hz, so a fast trackpad swipe
yields as few as 1 to 5 RUNNING-phase samples for the framework's velocity
tracker. With `PanZoomEnd` fired at RUNNING→INERTIA and inertia frames
suppressed, the framework declines to start a ballistic and the scroll stops
dead at finger lift. Fix: forward OS inertia frames as `PanZoomUpdate`, fire
`PanZoomEnd` at INERTIA→READY or RUNNING→READY, and handle INERTIA→RUNNING as
end-then-start, the same shape as macOS. Removes `OnScrollInertiaCancel`.

**PR needs:** the commit (tests included). This changes observable gesture
timing on Windows; expect a design discussion, and link #126649.

## 7. macOS: `Menu.setMenus` rebuilds the menu bar and dismisses an open menu

**Commit:** `[macOS] Reconcile the platform menu in place instead of rebuilding it`
**File:** `FlutterMenuPlugin.mm`
**Upstream:** unfixed; `setMenus` still assigns a freshly built `NSApp.mainMenu`.

**Issue text.** `FlutterMenuPlugin.setMenus` replaces `NSApp.mainMenu` with
a new tree on every call, re-creating every `NSMenuItem`. That re-mints the
ids the selection/open/close callbacks route through, resets item state, and
when a re-send arrives while a menu is open, tears down the menu the user is
using. Fix: build the tree once and reconcile later representations onto it
in place, matching by position and updating only changed attributes;
AppKit-injected items (the Help search field) are left untouched.

**PR needs:** the commit plus tests; the commit carries none, and upstream
will ask for `FlutterMenuPluginTest` cases (reconcile keeps an open submenu,
label update, add/remove at an index).

## 8. macOS: a missing platform view aborts the compositor in debug builds

**Commit:** part of `Add RGBA16Float (F16) compositing surface and EDR/HDR support for macOS and iOS` (the `FlutterCompositor.mm` hunk)
**Upstream:** unfixed; `PresentPlatformView` still `FML_DCHECK`s and `PresentPlatformViews` adds the result to an array unchecked.

**Issue text.** During a transition a layer's platform view lookup can return
nil; `PresentPlatformView` DCHECKs and the caller would `addObject:nil`,
crashing debug builds. Fix: return nil and skip the layer.

**PR needs:** split out of the EDR commit into its own commit; small enough
to be test-exempt, or add a `FlutterCompositorTest` case.

## 9. iOS: spurious vsync DCHECK in unoptimized debug builds

**Commit:** part of the same EDR commit (the `vsync_waiter.cc` hunk)
**Upstream:** unfixed; `FireCallback` still DCHECKs `Now() >= frame_start_time` on every platform.

**Issue text.** The iOS display link reports `frame_start_time` on a clock
epoch that differs from `fml::TimePoint::Now()` in unoptimized debug builds,
so the DCHECK fires spuriously. The carried fix compiles the check out on
iOS; upstream may prefer normalising the timestamp in the iOS vsync waiter.

**PR needs:** split out of the EDR commit; confirm the cause on current iOS
before filing, since upstream will ask which clock the display link used.

## 10. Raster cache offscreen format follows the destination surface

**Commit:** `Match raster cache offscreen format to the destination surface`
**Upstream:** unfixed, but only observable once a non-N32 destination exists, so it belongs with the F16 compositing-surface submission rather than on its own.

## 11. Build: `pipes` module removed in Python 3.13

**Commit:** part of `[android] F16/EDR compositing + direct-AHardwareBuffer producer path` (the `engine/src/build/android/gyp/util/build_utils.py` hunk)
**Upstream:** unfixed; master still does `import pipes` and `pipes.quote`.

**Issue text.** `build/android/gyp/util/build_utils.py` imports `pipes`,
which Python 3.13 removed, so an Android engine build on a host with
Python 3.13 fails in the gyp helpers. `shlex.quote` is the documented
replacement and the module already imports `shlex`.

**PR needs:** split the one-line hunk out of the EDR commit. Trivial,
test-exempt.

## Formatting status

Checked 2026-10-13 with the engine's own tools: clang-format (buildtools),
`gn format`, google-java-format 1.7, `dart format`, header guards and
trailing whitespace are clean for every hand-written file on the branch.
The one exception is `shell/platform/linux/wayland_vendor/`: those are
`wayland-scanner` outputs and fail clang-format by thousands of lines.
Upstream's `ci/bin/format.dart` skips any path containing `third_party`,
so a submission of the Linux HDR commit moves them under such a path (for
example `shell/platform/linux/third_party/wayland/`) or generates them at
build time.

## Not candidates

- The HCPP transaction fixes on this branch are cherry-picks of upstream
  #181009, #190612, #192606 and #192969; nothing to file.
- `Fix Xcode 27 lipo verification failure for Darwin frameworks (#188625)` and
  the two AHB swapchain commits by Jason Simmons are upstream commits.
- The F16/EDR/HDR compositing stack itself (macOS, iOS, Android, Windows,
  Linux) is the branch's main body and would be an upstream design
  conversation, not a bug report; it is tracked outside this file.
