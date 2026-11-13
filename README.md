# sfwm

sfwm is a native C++17 Wayland compositor and window manager for Android. It presents
Wayland surfaces through SurfaceFlinger layers, using SurfaceComposerClient transactions
for layer management and a GPU composition path for client buffers.

It targets Android devices where a conventional DRM/KMS compositor cannot own the display.
The core runs as a Termux process and does not require DRM master or a virtual terminal.

## Design

- **Display:** SurfaceFlinger owns the physical display. sfwm creates and manages its layers
  through runtime-bound SurfaceComposerClient symbols. It does not link against `libgui`
  or vendor AOSP headers.
- **Rendering:** client dma-bufs are imported as EGLImages and composited into the layer
  buffer. Zero-copy output is the normal path; CPU read-back is available as a fallback.
- **Window management:** floating windows, gestures, workspaces and optional dwindle/master
  tiling. Wayland clients and hosted Android windows share the layout model.
- **Input:** evdev touch, keyboard and pointer input, plus Wayland input-method support.
- **Android integration:** optional LSPosed bridge, SurfaceFlinger dma-buf helper and
  KernelSU service scripts. The compositor core does not require these modules.

SurfaceComposerClient is a private Android interface. The current implementation targets
arm64; framework changes may require updates to the isolated ABI wrapper.

## Build

Build in Termux with Clang, Meson and Ninja:

```sh
CC=clang CXX=clang++ meson setup build --prefix="$PREFIX"
ninja -C build
```

The executable is `build/sfwm`. The project is assembled as a single translation unit;
preserve the include order in `src/main.cpp` when changing the module structure.

## Run

Create a runtime directory and start sfwm with the target panel dimensions:

```sh
mkdir -p "$PREFIX/tmp/sfwm"
XDG_RUNTIME_DIR="$PREFIX/tmp/sfwm" ./build/sfwm --size=1272x2800 --scale=3
```

Clients use `XDG_RUNTIME_DIR="$PREFIX/tmp/sfwm"` and `WAYLAND_DISPLAY=sfwm-0`.
The size and scale above are reference-device values; set them for the target display.

## Documentation

- [Usage and control socket](doc/usage.md)
- [Android bridge](doc/bridge.md)
- [Graphics and driver notes](doc/driver-notes.md)
- [Nested output investigation](doc/nested-output.md)
- [Tiling](doc/tiling.md)
- [Reference-device setup](docs/ONDEVICE.md)
- [Development roadmap and known work](TODO.md)

## Status

sfwm is experimental and device-dependent. The core compositor, Android integration paths
and graphics backends have different maturity levels. See [TODO.md](TODO.md) for tracked
bugs, planned work and investigations. Device testing is required for SurfaceFlinger ABI
calls, input dispatch, buffer synchronization and Android-specific behaviour.

## License

AGPL-3.0-or-later. Third-party protocol definitions and linked libraries retain their
respective licenses.
