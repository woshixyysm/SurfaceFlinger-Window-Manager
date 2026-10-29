# deps

Termux runtime libraries used by the CI cross build.

`termux-deps-aarch64.tar.xz` (432 KB) is packed directly from the Termux prefix on the
development device:

| library | version |
|---|---|
| wayland (server) | 1.25.0 |
| libxkbcommon | 1.13.2 |
| libdrm | 2.4.134 |
| device | OnePlus PLQ110, Android API 36, aarch64 with bionic |

It contains only what the build needs:

```
include/wayland*.h         wayland-server-core.h, wayland-server-protocol.h, wayland-util.h
include/xkbcommon/         xkbcommon.h
include/xf86drm.h libdrm/ drm/
lib/libwayland-server.so*  lib/libxkbcommon.so*  lib/libdrm.so
```

## Why binaries are committed

The CI runner is glibc on x86_64 and cannot target bionic on aarch64, while sfwm links
against these three libraries as they exist on the device. Cross compiling them is a
separate project, and downloading them from the Termux repository on every run adds the
risk that package names or the index change. Packing the device copy keeps the ABI
identical to the runtime and removes the external dependency.

## Regenerating the archive

On the device, using `tools/pack-deps.sh`:

```sh
adb push tools/pack-deps.sh /data/local/tmp/pack-deps.sh
adb shell "su 0 sh /data/local/tmp/pack-deps.sh"
adb pull /data/local/tmp/termux-deps-aarch64.tar.xz deps/
```

The script exports `PATH=$PREFIX/bin:/system/bin` before packing. A shell started by `su`
otherwise picks up the busybox `xz`, which has no `-9`, and the busybox `tar`.

## Fallback

`build-aux/ci-fetch-termux-deps.sh` fetches the same files from the Termux repository when
no device is available, by parsing `Packages.xz` and matching package names exactly. CI
does not use it; it is kept for emergencies.
