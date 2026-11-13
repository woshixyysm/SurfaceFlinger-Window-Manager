# sfwm

sfwm is a native C++17 Wayland compositor and window manager for Android. It runs as a
Termux process and uses SurfaceFlinger for final display composition.

Each Wayland surface is represented by a SurfaceFlinger layer. sfwm manages the layer's
position, geometry, stacking order and visibility through SurfaceComposerClient transactions.
The client buffer is presented through that layer's BufferQueue. SurfaceFlinger remains the
owner of the physical display.

The project is intended for Android devices where a conventional DRM/KMS compositor cannot
own the display. It does not require DRM master, a VT or a modesetting backend.

## Design

### SurfaceFlinger integration

The compositor binds the required SurfaceComposerClient symbols at runtime with dlopen and
dlsym. The core does not vendor AOSP headers and does not link against libgui. The ABI
definitions and AArch64 calling-convention bridges live in src/compositor/sf_layer.hpp.

This is a private Android ABI. The current implementation targets arm64 and has been
measured on Android 16. Framework updates can change symbol names, object layouts or method
signatures. Keep those assumptions isolated in the SurfaceFlinger layer wrapper.

### Rendering

The normal path imports a client dma-buf as an EGLImage, samples it through the GPU and
composites into the SurfaceFlinger layer buffer.

SFWM_ZC is enabled by default. The SurfaceFlinger buffer's dma-buf is used as the render target.
This avoids the CPU read-back. If that path cannot be used, sfwm falls back to locking the
layer buffer and reading the GPU result into it. SFWM_GL=0 selects the CPU copy path.

SFWM_ASYNC=1 calls the private ANativeWindow swap-interval entry point with interval zero.
This enables async BufferQueue mode and removes the producer's wait for a free buffer. It
raises submitted frame throughput and reduces the associated input delay. It does not change
the panel's physical refresh rate.

Three alternate presentation paths are available:

- SFWM_DIRECT enables an experimental direct-buffer path through ASurfaceControl. Unsupported
  formats fall back to the normal compositor path.
- SFWM_SF_HOOK enables the optional KernelSU helper inside SurfaceFlinger. SurfaceFlinger
  imports the client's dma-buf directly. This path is intended for a single fullscreen client.
  Leave it disabled for multi-window use.
- SFWM_PRESENT_BRIDGE sends frames to the app-owned presentation host packaged with the
  Android bridge. It is a fallback path and is not used by the reference configuration.

### Input and window management

sfwm reads the touchscreen through evdev and translates contacts into Wayland pointer or
touch events. The default startup does not take exclusive ownership of the device.
--grab enables EVIOCGRAB when exclusive input is required. The --clickthrough option lets
touches reach the Android UI below the compositor layers. The --touch option enables
wl_touch delivery.

The floating window manager supports moving, corner resizing, pinch zoom, snapping,
workspaces and a control socket. Tiling is optional and starts disabled. Its dwindle and
master layouts share one ordering model for Wayland windows and hosted Android windows.

### Android integration

The Android modules are optional. The core compositor starts without them.

The LSPosed bridge in android/ provides framework capabilities through a small line protocol.
It can report input-method state and text events, expose virtual-display slots, host Android
windows and start the system pointer sprite. The display and input-observation capabilities
are disabled by default. Configuration and failure behaviour are documented in doc/bridge.md.

Hosted Android windows are managed by Android's window manager. System UI can appear above
them, and their z-order cannot be interleaved freely with Wayland layers.

The optional KernelSU module in android/ksu/ adds the SurfaceFlinger-side dma-buf import
helper. It is off by default and can be removed without affecting the normal compositor path.

The optional service scripts in android/ksu-service/ expose sfwm-service for starting,
stopping and inspecting the compositor. Runtime state is kept under /dev/sfwm by default.

## Repository layout

~~~
src/
  main.cpp                 single translation unit and module include order
  core/                    shared configuration, types and tracing
  compositor/              SurfaceFlinger layers, surfaces, views and tiling
  render/                   EGL composition, direct presentation and cursor rendering
  input/                    seat, keyboard, pointer, touch and injection
  protocol/                 Wayland protocol implementations
  shell/                    xdg-shell and layer-shell
protocols/                  vendored protocol XML
android/                    optional LSPosed bridge and native presentation hosts
android/ksu/                optional SurfaceFlinger import helper
android/ksu-service/        optional service scripts and configuration
build-aux/                  protocol generation and cross-build helpers
deps/                       Termux headers and libraries used by CI
doc/                        usage, driver, bridge, nesting and tiling notes
docs/ONDEVICE.md             reference-device setup and measurements
tools/                      structure checker and device helpers
~~~

The project is built as one translation unit. Modules share static globals and depend on
the include order in src/main.cpp. Do not split the source into separate translation units
without first changing those dependencies.

## Build

Build the compositor in Termux on Android. The source uses bionic, libandroid, liblog,
Wayland server libraries, libdrm and xkbcommon.

~~~sh
CC=clang CXX=clang++ meson setup build --prefix="$PREFIX"
ninja -C build
~~~

Meson configures C++17 by default. Set CC and CXX explicitly so a GCC toolchain earlier on
PATH does not replace the intended Clang compiler. The build also uses the Wayland protocol
XML files installed under $PREFIX/share/wayland-protocols and the vendored files under
protocols/.

The resulting executable is build/sfwm.

### Android modules

Build the LSPosed bridge APK with PowerShell:

~~~powershell
pwsh -File android/build.ps1
~~~

The build needs an Android SDK, an NDK, JDK 17 or newer and kotlinc. It packages the Kotlin
bridge, the system cursor sprite host and the app-owned presentation host.

Build the optional SurfaceFlinger hook module with:

~~~powershell
pwsh -File android/ksu/build.ps1
~~~

Install the resulting ZIP through KernelSU or Magisk. The hook is an experimental
acceleration path. Keep it disabled unless the target device and workload have been tested.

## Run

The default output dimensions are 1272x2800 and the default scale is 3. Set the output size
explicitly when using a different panel.

~~~sh
export XDG_RUNTIME_DIR=/data/local/tmp/sfwm-xdg
mkdir -p "$XDG_RUNTIME_DIR"
./build/sfwm --size=1272x2800 --scale=3 --z=1000 --socket=sfwm-0
~~~

The process needs permission to use SurfaceComposerClient and read the selected input
device. On the reference device it runs under the Termux UID with a device-specific SELinux
label adjustment through ctxfix. The exact setup depends on the Android build and its policy.

| Option | Default | Purpose |
| --- | --- | --- |
| --size=WxH | 1272x2800 | Physical output size |
| --scale=N | 3 | Logical output scale |
| --z=N | 1000 | Base z-order for content layers |
| --socket=NAME | sfwm-0 | Wayland socket name |
| --grab | off | Take exclusive access to the touch device |
| --clickthrough | off | Let input reach Android below the compositor layers |
| --touch | off | Deliver wl_touch events to clients |

sfwm ignores unknown arguments. It does not provide a --help option. See doc/usage.md for
the command socket and environment-variable reference.

### Useful environment variables

| Variable | Effect |
| --- | --- |
| SFWM_GL=0 | Use the CPU copy path |
| SFWM_ZC=0 | Disable zero-copy output and use the read-back path |
| SFWM_ASYNC=1 | Enable async BufferQueue mode |
| SFWM_FPS=N | Set the layer frame-rate vote and hook pacing limit |
| SFWM_DIRECT=1 | Try the experimental direct-buffer path |
| SFWM_SF_HOOK=1 | Use the optional KernelSU helper inside SurfaceFlinger |
| SFWM_PRESENT_BRIDGE=1 | Use the app-owned presentation host |
| SFWM_TRACE=1 | Log geometry, hit-testing and gesture details |
| SFWM_FRAMETIME=1 | Log presentation timing |
| SFWM_DPI=N | Set the logical DPI reported through wl_output |
| SFWM_IME_RESERVE=1 | Reserve output space for the input method |
| SFWM_LIBEGL | Select the EGL library |
| XCURSOR_THEME, XCURSOR_PATH, XCURSOR_SIZE | Select the cursor theme |

The service configuration uses SFWM_ASYNC=1 and SFWM_ZC=1 on the reference device. The
interactive compositor defaults remain suitable for running without the service module.

## Wayland support

The advertised protocol set includes:

- Core: wl_compositor, wl_subcompositor, wl_shm, wl_seat, wl_output and wl_data_device_manager.
- Shells: xdg_wm_base, zwlr_layer_shell_v1 and xwayland_shell_v1.
- Buffers and presentation: zwp_linux_dmabuf_v1, wl_drm, wp_presentation, wp_fifo_v1,
  wp_commit_timing_manager_v1 and wp_linux_drm_syncobj_manager_v1 when the driver supports it.
- Input: text-input-v3, input-method-v2, virtual-keyboard, pointer constraints, relative pointer,
  pointer gestures, virtual pointer, idle inhibit and keyboard shortcuts inhibit.
- Desktop integration: foreign toplevel lists, workspace management, session lock,
  activation, idle notification and toplevel icons.
- Capture and display control: screencopy, image capture, gamma control and output power.

Protocol availability can depend on driver capabilities. For example, timeline syncobj support
is probed before wp_linux_drm_syncobj_manager_v1 is advertised.

## Testing

Run the source structure check before submitting changes:

~~~sh
python3 tools/check_structure.py
~~~

CI also cross-builds against the Termux headers and libraries stored in deps/. The structure
check validates local includes, the single-translation-unit module graph, header guards and
protocol generation.

Device testing is still required for SurfaceFlinger ABI calls, input dispatch, dma-buf
synchronisation, EGL imports, async BufferQueue behaviour and the optional Android modules.
A successful cross-build does not verify those runtime paths.

## Documentation

- doc/usage.md covers command-line options, environment variables and the control socket.
- doc/driver-notes.md records Android graphics, dma-buf, EGL and input behaviour.
- doc/bridge.md describes the LSPosed bridge, its capabilities and wire protocol.
- doc/nested-output.md records the virtual-display investigation and its measured limits.
- doc/tiling.md describes the tiling model and commands.
- docs/ONDEVICE.md records the reference device setup, measurements and service workflow.
- android/ksu/README.md describes the optional SurfaceFlinger import helper.

The nested-output document contains investigation results and design alternatives. Treat each
route according to its recorded status. Do not read a proposed route as an implemented feature.

## Reference environment

The development reference is a OnePlus PLQ110 running Android 16 on arm64 with an Adreno 830.
The tested graphics stack is a locally modified Mesa 26.2.5 tree derived from the
mesa-for-android-container fork. Its KGSL/freedreno changes are device-specific. The reported
OpenGL 4.6 core profile belongs to that build. Android's bundled EGL implementation reports
a different capability set.

## Current constraints

- The core compositor has one physical output and one Wayland seat.
- Android windows hosted by the bridge follow Android window-manager stacking rules.
- The hardware pointer sprite is used for hosted Android windows. Wayland windows use the
  compositor's own cursor layer.
- Tiling has no animations or per-window rules.
- xdg_toplevel_icon requests are accepted, but icon contents are not stored.
- The screencopy path still copies pixels into client memory.

## License

sfwm is licensed under the GNU Affero General Public License, version 3 or later.
Third-party protocol XML and linked libraries retain their own licenses.
