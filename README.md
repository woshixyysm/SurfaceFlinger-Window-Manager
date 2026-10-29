# sfwm

A Wayland compositor that uses SurfaceFlinger as its compositor.

Every `xdg_toplevel` and `wl_surface` maps to a SurfaceFlinger layer. Position, z-order
and visibility are applied through `SurfaceComposerClient` transactions, so SurfaceFlinger
performs the actual composition on the display hardware. sfwm itself only moves pixels
and implements window policy.

## Why SurfaceFlinger

The device this was written for exposes no DRM device node and has `CONFIG_VT` disabled.
Without DRM master, a conventional compositor such as wlroots or Hyprland cannot take
over the display. SurfaceFlinger is the only rendering path available, and it is driven
through its Binder interface rather than through DRM.

The ABI is bound at run time with `dlopen` and `dlsym`: no AOSP headers are vendored and
nothing links against `libgui`, so the binary depends only on the public Android NDK, the
Wayland libraries and libdrm.

## Repository layout

```
src/
  main.cpp                entry point; the include order below is significant
  core/
    prelude.hpp           standard, protocol and Wayland headers, shm fd helper
    config.hpp            configuration globals and the buffer, connection and server types
    trace.hpp             structured tracing (SFWM_TRACE)
  compositor/
    view.hpp              per-surface state, geometry and stacking
    surface.hpp           wl_compositor, wl_region, wl_surface and wl_shm
    sf_layer.hpp          SurfaceFlinger layer wrapper and ABI bridges
  render/
    gl_comp.hpp           GPU composition through EGLImage and an FBO
    cursor.hpp            cursor geometry, Xcursor themes, presentation
    testpat.hpp           self-test pattern (SFWM_TESTPAT)
  input/
    seat.hpp              wl_seat, pointer and touch bindings
    keyboard.hpp          wl_keyboard, text_input_v3, input method, control commands
    pointer.hpp           pointer, buttons, hit testing, raising
    touch.hpp             touch state machine, gestures, /dev/input
    inject.hpp            key injection socket
  protocol/               one header per protocol, listed below
  shell/
    xdg_shell.hpp         xdg_wm_base, toplevel, popup, configure
    layer_shell.hpp       zwlr_layer_shell_v1
protocols/                protocol XML vendored from wlroots and wayland-protocols
build-aux/                protocol generation and the CI cross build
tools/                    device-side helper scripts and the structure checker
deps/                     Termux headers and libraries for the CI cross build
doc/                      usage reference and driver notes
```

The project is built as a single translation unit. Each module shares static globals with
its neighbours, so `src/main.cpp` includes them in a fixed order and the order must not be
changed. Making every global an `extern` declaration is a prerequisite for splitting the
modules into separate translation units.

## Documentation

- [doc/usage.md](doc/usage.md): command line options, environment variables and every
  command the control socket accepts.
- [doc/driver-notes.md](doc/driver-notes.md): what Android and Adreno do that matters when
  compositing through SurfaceFlinger, covering presentation, buffer synchronisation,
  OpenGL ES and desktop GL, Vulkan, hardware video decode and input.

## Building

Building requires Termux on Android, since the code depends on bionic, `libandroid`,
`liblog` and the SurfaceFlinger Binder ABI.

```sh
CC=clang CXX=clang++ meson setup build --prefix=$PREFIX
ninja -C build
```

The compiler is named explicitly for reproducibility. A stock Termux installation uses
clang, which is also what `gcc`, `g++` and `c++` point at. The development device instead
has a separate GCC 17 toolchain installed ahead of clang on `PATH`, and that compiler
rejects the out of order designated initialisers used in the source, so `CC` and `CXX` are
set here rather than relying on the default. `--prefix` is required as well, because the
build uses it to locate the Wayland headers and libraries.

`build/sfwm` is the result.

### Reference environment

The numbers and log excerpts quoted in this file and in the source comments were taken on
the machine this was developed on, not on a generic device:

| item | value |
|---|---|
| device | OnePlus PLQ110, Android 16, `aarch64` |
| compiler | clang 21 from Termux (`gcc` and `g++` are symlinks to it) |
| graphics stack | Mesa 26.2.5 derived from the `mesa-for-android-container` fork, with local changes |
| GPU | Adreno 830 |

The graphics stack is not upstream Mesa. The freedreno gallium backend used here comes
from the `mesa-for-android-container` fork, which adds the KGSL driver under
`src/freedreno/drm/kgsl` that upstream does not carry. The tree was then modified locally
for this device, so the version string alone does not identify the code that was built.

The GL version a user sees depends on the EGL implementation that is loaded, so sfwm logs
it once at startup as `[gl] GPU composition ready: <GL_VERSION> | <GL_RENDERER>`. With the
build above the line reports a 4.6 core profile and `Adreno (TM) 830`. The driver shipped
with Android reports a different version, because it is a different GL implementation.

## Running

```sh
./build/sfwm --size=1272x2800 --scale=1 --z=1000 --socket=sfwm-0
```

| option | meaning |
|---|---|
| `--size=WxH` | physical size of the output |
| `--scale=N` | logical scale factor, applied to the compositor's own UI. Text size is better raised with `SFWM_DPI`; see below |
| `--z=N` | base z-order for content layers |
| `--socket=NAME` | Wayland socket name |
| `--grab` | take exclusive access to the touch device with `EVIOCGRAB` |
| `--clickthrough` | layers stop accepting input, so the Android UI remains reachable |
| `--touch` | forward `wl_touch` to clients instead of pointer events |

The process is normally started as root, because it reads `/dev/input` and creates
SurfaceFlinger layers. The Wayland socket is made world writable so that clients running
as an application user can connect; the runtime directory itself belongs to that user.

### Starting it on the reference device

The session script on the development machine starts the compositor like this. Everything
after the binary name is part of this machine's setup and is described below, so it is a
record of one working configuration rather than a requirement:

```sh
setsid env HOME=/data/data/com.termux/files/home \
    XDG_RUNTIME_DIR=/data/local/tmp/sfwm-xdg \
    LD_LIBRARY_PATH=$PREFIX/lib \
    LD_PRELOAD="/data/local/tmp/libeglcompat.so /data/local/tmp/libfdshim.so \
                /system/lib64/libnativewindow.so /system/lib64/libbinder_ndk.so \
                /system/lib64/liblzma.so" \
    SFWM_LIBEGL=$PREFIX/lib/libEGL.so.1 \
    MESA_LOADER_DRIVER_OVERRIDE=msm \
    LIBGL_DRIVERS_PATH=$PREFIX/lib/dri \
    ./sfwm --size=1272x2800 --z=1000 --scale=1
```

- `XDG_RUNTIME_DIR` is a directory writable by both the compositor and the clients. A
  directory under `/data/local/tmp` is used because the two run as different uids.
- `LD_LIBRARY_PATH` points at the Termux prefix, which is where the Wayland and Mesa
  libraries live. The process is started with `env -u LD_LIBRARY_PATH` when the system
  libraries have to stay visible instead.
- `LD_PRELOAD` carries the two shims this device needs: one maps the EGL entry points that
  `libgui` expects onto the core names, the other redirects the DRM node to the KGSL node
  and reports the driver name. The `/system/lib64` entries are the libraries those shims
  link against. None of this is part of sfwm.
- `SFWM_LIBEGL`, `MESA_LOADER_DRIVER_OVERRIDE` and `LIBGL_DRIVERS_PATH` select the Mesa
  build and the msm driver, so that the GPU path is used instead of the CPU copy.
- `--scale=1` with `SFWM_DPI` set instead is deliberate. Raising `--scale` shrinks the
  logical space a client sees, which pushes windows larger than the output; raising the
  reported DPI enlarges text while leaving the logical space intact.

A start-up on that machine logs the display handshake, the protocol globals, the input
device, the injection sockets and the keymap, in this order:

```
[sfwm] output 1272x2800 (logical 1272x2800), scale=1, base_z=1000, socket=sfwm-0
[sf] SurfaceComposerClient = 0x...
[screencopy] global registered (1272x2800, shm XRGB8888)
[syncobj] driver has no DRM_CAP_SYNCOBJ_TIMELINE, protocol not advertised
[out] reporting physical size 192x423 mm (dpi 168, output 1272x2800)
[input] /dev/input/event6 'touchpanel' x[0,20351] y[0,44799]
[kbd] injection socket: .../sfwm-keys.sock
[kbd] keymap ready
```

A client connecting afterwards logs `[xwl] Xwayland bound xwayland_shell_v1`,
`[input] new pointer` and `[kbd] new client bound the keyboard`, and `dump` on the control
socket answers with the output size, the gesture owner and the pointer state.

## Protocols

Clients are given the protocols they bind at startup, including:

- core Wayland: `wl_compositor`, `wl_subcompositor`, `wl_shm`, `wl_seat`, `wl_output`,
  `wl_data_device_manager`
- shell: `xdg_wm_base`, `zwlr_layer_shell_v1`, `xwayland_shell_v1`
- buffers: `zwp_linux_dmabuf_v1` with feedback, `wl_drm`, `wp_single_pixel_buffer_v1`
- presentation: `wp_presentation`, `wp_fifo_v1`, `wp_commit_timing_manager_v1`,
  `wp_linux_drm_syncobj_manager_v1` when the driver supports timeline syncobj
- scaling and appearance: `wp_viewporter`, `wp_fractional_scale_manager_v1`,
  `wp_cursor_shape_manager_v1`, `wp_alpha_modifier_v1`, `wp_content_type_manager_v1`,
  `wp_tearing_control_manager_v1`, `zxdg_output_manager_v1`, `zxdg_decoration_manager_v1`
- input: `zwp_text_input_manager_v3`, `zwp_input_method` through the vendored
  `input-method-unstable-v2`, `virtual-keyboard-unstable-v1`, `zwp_pointer_constraints_v1`,
  `zwp_relative_pointer_manager_v1`, `zwp_pointer_gestures_v1`, `wp_pointer_warp_v1`,
  `zwp_idle_inhibit_manager_v1`, `zwp_keyboard_shortcuts_inhibit_manager_v1`,
  `zwlr_virtual_pointer_manager_v1`
- clipboard: `zwp_primary_selection_device_manager_v1`, `ext_data_control_manager_v1`,
  `zwlr_data_control_manager_v1`
- desktop integration: `ext_foreign_toplevel_list_v1`, `zwlr_foreign_toplevel_manager_v1`,
  `xdg_toplevel_icon_manager_v1`, `ext_workspace_manager_v1`, `ext_session_lock_manager_v1`,
  `ext_idle_notifier_v1`, `xdg_activation_v1`
- capture: `zwlr_screencopy_manager_v1`, `ext_output_image_capture_source_manager_v1`,
  `ext_image_copy_capture_manager_v1`
- output control: `zwlr_gamma_control_manager_v1`, `zwlr_output_power_manager_v1`

## Compositing backends

The default path imports the client dmabuf as an EGLImage, draws it into an FBO on the
GPU, and reads the result back into the SurfaceFlinger layer buffer. This keeps the dma-buf
fence handling and the stride arithmetic on the driver, and every destination pixel is
written, so no unpainted area is left for SurfaceFlinger to blend.

Setting `SFWM_GL=0` selects the CPU path, which maps the dmabuf and copies it. It is kept
for comparison and as a fallback when the EGL display cannot be created.

## Environment variables

| variable | effect |
|---|---|
| `SFWM_GL=0` | use the CPU copy path |
| `SFWM_GL_FLIP` | vertical flip for the GPU path |
| `SFWM_GLDBG` | log buffer geometry changes on the GPU path |
| `SFWM_TESTPAT=1` | draw the self-test pattern over every frame |
| `SFWM_TRACE=1` | write geometry, hit test and gesture records to a log |
| `SFWM_FRAMETIME` | log per frame timing |
| `SFWM_NO_DEFER` | read each shm frame immediately instead of one frame later |
| `SFWM_DPI=N` | logical DPI reported through `wl_output` |
| `SFWM_IME_RESERVE=1` | reserve the bottom of the output for the keyboard |
| `SFWM_LIBEGL=/path/libEGL.so.1` | EGL library to load, ahead of the built-in search order |
| `XCURSOR_THEME`, `XCURSOR_PATH`, `XCURSOR_SIZE` | cursor theme lookup |

## Control socket

A unix socket accepts line commands, used by `sfwmc` and by the device scripts. It accepts
text input, raw evdev key codes, keysym names, synthetic clicks, window moves, workspace
switches, brightness, colour temperature, display power, click-through toggling and zoom.
The command set is listed by the `help` command.

There is no physical keyboard on the target device, so typing goes through this socket.
Text is delivered with `commit_string` when the focused client supports `text_input_v3`,
which is what makes CJK input work; key codes alone cannot express it.

## Device helpers

`tools/` contains scripts that run on the device with root:

- `pack-deps.sh` packs the Termux headers and libraries used by CI
- `kill-heavy.sh` force-stops a list of packages and reports memory
- `quiesce.sh` restricts a list of packages so they cannot restart themselves

Manual work in the Android UI, such as stopping the launcher or enabling freeform
windows, is left to the user rather than scripted here.

## Testing

`tools/check_structure.py` verifies that every local include resolves, that every header is
reachable from `src/main.cpp`, that the module graph has no cycles, that every header has
`#pragma once`, that no generated file is tracked, and that the protocol list in
`meson.build` can be generated with `wayland-scanner`.

CI runs that check on every push. It also performs a cross build against the Termux
headers in `deps/`, because the project itself only compiles on Android. Both jobs are
required: a change that breaks the cross build fails the workflow rather than turning a
job red in the background.

The cross build is what keeps the source honest about portability, and it has caught two
problems that the development device could not show:

- `text-input-unstable-v3` gained three requests in version 2. A distribution copy older
  than that generates an interface struct without them, so the protocol XML is vendored
  under `protocols/` instead of being taken from `wayland-protocols` at build time;
- `glob()` is not declared by every NDK header set, so the backlight lookup scans
  `/sys/class/backlight` with `opendir` and `readdir` instead.

What has been checked by hand on the device, beyond what CI can reach: the binary starts
and binds SurfaceFlinger, registers the protocol globals, reads the touch device, opens the
injection sockets and sends the keymap; Xwayland connects and binds `xwayland_shell_v1`,
the seat and the keyboard; and the control socket answers `dump` with the output size, the
gesture owner and the pointer position.

## Limitations

- Single output, single seat, and no hardware cursor plane.
- Windows are moved and resized with gestures owned by the compositor rather than by
  client decorations.
- Screenshot and recording support runs `screencap` and copies the result, so it is not a
  zero copy path.
- Icons set through `xdg-toplevel-icon-v1` are accepted but not stored.

## Prior art

sfwm was written by reading the existing Wayland compositors and their protocol
implementations, not from the protocol reference alone. Where a behaviour follows another
implementation, the comment at that point names it.

- **Weston**: protocol semantics. Selection is only sent to clients that hold keyboard
  focus, and the data device and primary selection model follows its arrangement.
- **wlroots and sway**: the protocols in `protocols/` are vendored from wlroots, and their
  requests are handled the way wlroots based compositors expect. That covers
  `zwlr_layer_shell_v1`, `zwlr_screencopy`, `zwlr_foreign_toplevel_manager`,
  `zwlr_data_control`, `zwlr_virtual_pointer`, `zwlr_gamma_control` and
  `zwlr_output_power_manager`.
- **Hyprland**: the touch state machine, the decision to fix the owner of a gesture until
  release, and the composer side zoom that changes only the layer geometry instead of
  sending a configure.
- **niri and quickshell**: the workspace and toplevel list protocols are implemented
  against what their bars and taskbars bind.
- **Mesa and turnip**: importing a dmabuf and calling into KGSL follow the order used in
  `tu_knl_kgsl.cc`. The freedreno KGSL gallium backend itself comes from the
  `mesa-for-android-container` fork.
- **Android**: the layer flag values and the transaction ABI follow
  `frameworks/native/include/gui/LayerState.h` and `SurfaceComposerClient`.

The protocol XML files under `protocols/`, and the Wayland, xkbcommon, libdrm and Mesa
libraries this links against, are third party code and remain under their own licences.

### Other projects that run Wayland on Android

These were read before writing the Android side of this compositor, and they solve parts
of the same problem in different ways. None of their source code is used here; the only
code taken from another tree is the protocol XML under `protocols/`. Their licences are
listed because they define the state of the art this builds on.

- **[wldroid](https://github.com/Shelnutt2/wldroid)**, MIT. A wlroots compositor packaged
  as Android library modules, with a proot managed rootfs, VirGL and Venus GPU
  translation, and DRM, GBM and EGL shim libraries. It embeds the compositor in an app and
  composes into an AHardwareBuffer.
- **[anland](https://github.com/SuperTurtleDev/anland)**, GPL-3.0. Runs container
  applications as native Android windows: one Activity and Surface per `xdg_toplevel`
  rather than one virtual screen, with clients connected over binder instead of a socket.
  It offers a SurfaceControl backend that leaves composition to SurfaceFlinger and an EGL
  backend that composites per window.
- **[wlroots-android-bridge](https://github.com/Xtr126/wlroots-android-bridge)**, GPL-3.0.
  Drives a wlroots compositor, now labwc, from an app over AIDL, allocating through
  minigbm gralloc handles and presenting each window with SurfaceControl transactions
  instead of an EGL swapchain.
- **[piperella-wayland-compositor](https://github.com/Piperella/Piperella-Wayland-Compositor)**,
  GPL-3.0. Runs `libwayland-server` inside an Android app, composites with EGL and GLES3
  into a SurfaceView with a zero copy dmabuf path, and paces rendering on Choreographer.
  Its binder input path is derived from wlroots-android-bridge.
- **[droidloom](https://github.com/denialwm/droidloom)**, GPL-3.0-or-later. The opposite
  direction: it runs Android applications inside a Linux Wayland session, which is useful
  for the parts of this problem that concern input and clipboard bridging.
- **[termux-wayland](https://github.com/HomuHomu833/termux-wayland)**, GPL-3.0. A fork of
  Termux:X11 with the X server replaced by a Wayland compositor. It draws all windows with
  GLES into one AHardwareBuffer per refresh, and the Android app displays that buffer and
  feeds input back.

sfwm takes a different route from all of them. It is not embedded in an application and
does not hand a single buffer to an app: it runs as a privileged process next to
SurfaceFlinger, creates one SurfaceFlinger layer per surface, and writes into that layer
buffer directly. That needs root, which the projects above avoid, and in exchange it needs
neither a shim layer for the Android graphics stack nor a compositor inside the app
process.

## Development

The code was written with a large language model used as an assistant, for drafting,
reviewing and translating comments and messages. It was not given free rein: every change
was compiled and exercised on the device, and the behaviour of each protocol was checked
against the client that uses it. The comment blocks state why an approach was taken rather
than only what the code does, which is what makes that review possible.

## License

Copyright (C) 2026 the sfwm authors.

sfwm is free software: you can redistribute it and/or modify it under the terms of the GNU
Affero General Public License as published by the Free Software Foundation, either version
3 of the License, or (at your option) any later version.

sfwm is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU Affero General Public License for more details.

The full text is in `LICENSE`.
