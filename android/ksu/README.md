# sfwm SurfaceFlinger hook module

**Optional.** Like the [LSPosed bridge](../README.md), sfwm runs fine without this: it is
only an experiment for removing the last composite step. If anything misbehaves, disable or
remove the module and reboot.

```powershell
pwsh -File android/ksu/build.ps1
# then install android/ksu/build/sfwm-sfhook.zip with KernelSU / Magisk and reboot
```

## Why it exists

sfwm composites a client's dma-buf into the SurfaceFlinger buffer with a GPU blit. Handing
the dma-buf to SurfaceFlinger as-is (no blit at all) needs
`AHardwareBuffer_createFromHandle()` with the **IMPORT** method, and that call always fails
with `EINVAL` from any process other than SurfaceFlinger:

```
AHardwareBuffer_allocate              -> 0      (allocator is a binder service, reachable)
AHardwareBuffer_recvHandleFromUnixSocket -> 0   (CLONE method, no mapper involved)
AHardwareBuffer_createFromHandle(IMPORT) -> -22 (needs the mapper)
```

`service list` shows only `android.hardware.graphics.allocator.IAllocator/default`; there is
no mapper service. SurfaceFlinger uses the **passthrough** HAL module
`/vendor/lib64/hw/android.hardware.graphics.mapper@4.0-impl-qti-display.so` inside its own
process, which is why the import only works there.

## What it does

A wrapper replaces `/system/bin/surfaceflinger`, sets `LD_PRELOAD` and execs the stock binary
saved at install time. The preloaded library starts one tiny detached thread that listens on
`/data/adb/sfwm-sf/sf.sock` and does exactly one thing:

```
request : magic,width,height,fourcc,stride  + one SCM_RIGHTS fd   (the client dma-buf)
response: status, then (on success) an AHardwareBuffer sent with
          AHardwareBuffer_sendHandleToUnixSocket()
```

sfwm reads it back with `AHardwareBuffer_recvHandleFromUnixSocket()`, which uses CLONE and
needs no mapper, and sets that buffer on its own layer with
`ASurfaceTransaction_setBuffer()`. All the presentation logic stays in sfwm.

## Keeping SurfaceFlinger alive

The helper is written so that it cannot take the system UI down:

- it never calls a SurfaceFlinger symbol, links against its internals, or touches its state
- every failure path answers with an error and returns; sfwm then falls back to its blit path
- it exits immediately unless `SFWM_SF_HOOK=1` is set, which only the wrapper does
- the serving thread gets a 128 KiB stack and no exceptions (`-fno-exceptions`)
- the wrapper execs the stock binary even when the helper is missing, so a broken install
  still boots

It can still restart the system UI if the import path itself misbehaves inside
SurfaceFlinger. That is why the module is off by default and marked optional.

## Status

The module and the helper build and install; the round trip has **not** been proven to fix
the EINVAL yet, because that depends on whether the vendor mapper accepts a dma-buf that did
not come from its own allocator even when the call is made inside SurfaceFlinger. Test with:

```sh
# in sfwm, enable the client
SFWM_SF_HOOK=1 ./sfwm ...
# the log should show  [direct] sf-hook import ok  instead of the EINVAL fallback
```

If the import still fails inside SurfaceFlinger, the conclusion is that Android's glalloc
simply does not allow wrapping a foreign dma-buf, and only a client that renders into a
gralloc buffer (the way a launcher does) can avoid the extra pass.

## Layout

```
module.prop                 opt-in metadata shown by the module manager
customize.sh                installer; saves the stock surfaceflinger binary
sepolicy.rule               lets surfaceflinger read the module dir and its socket
src/sfwm_sf_hook.c          the preloaded helper
src/surfaceflinger_wrapper.c  small static wrapper that sets LD_PRELOAD and execs the stock one
system/bin/surfaceflinger   built wrapper
lib/libsfwmsfhook.so        built helper
build.ps1                   NDK build + zip packaging
```
