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
process, which is why the import only works there. (It does work there — see Status.)

## What it does

A wrapper replaces `/system/bin/surfaceflinger`, sets `LD_PRELOAD` and execs the stock binary
saved at install time. The preloaded library starts one tiny detached thread that listens on
the **abstract** socket `@sfwm-sf` and does exactly one thing:

```
request : magic,width,height,fourcc,stride  + one SCM_RIGHTS fd   (the client dma-buf)
response: status, then (on success) an AHardwareBuffer sent with
          AHardwareBuffer_sendHandleToUnixSocket()
```

An abstract socket is used because `/data/adb` is `0700 root` and SurfaceFlinger runs as
uid 1000, so a filesystem socket is not reachable no matter what the SELinux labels say.
The helper additionally checks `SO_PEERCRED` and only answers uid 0 and the Termux uid.

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

**The import works, and presentation is now a pure scanout of the client's own dma-buf.**
`sfwm` logs `[direct] sf-hook import ok, scanout without compositing` and the GPU blit is
skipped entirely.

Getting there took a disassembly round. The verdict of `AHardwareBuffer_createFromHandle()`
was misleading, so the call chain was read instruction by instruction:

| where | what it does | returns |
| --- | --- | --- |
| `libnativewindow.so AHardwareBuffer_createFromHandle+0x34` | `desc.rfu0 != 0 \|\| desc.rfu1 != 0` | `-EINVAL` |
| `libui.so GraphicBuffer::initWithHandle+0x98` | `GraphicBufferMapper::importBuffer()` → mapper HAL | `-EINVAL` |
| `…mapper@4.0-impl-qti-display.so QtiMapper::validateBufferSize+0x134` | `BufferManager::ValidateBufferSize(handle, info)` | `BAD_VALUE` → `-EINVAL` |

`logcat` narrowed it down (`Gralloc5: Format didn't match, expected 5 got UNSPECIFIED`),
so the failure was never "this dma-buf is foreign". Two real bugs in the helper caused it:

1. the placeholder buffer was allocated with `usage = COMPOSER_OVERLAY` while the import
   descriptor said `usage = CPU_READ | COMPOSER_OVERLAY`, so `validateBufferSize` compared
   two different usages;
2. **all** `numFds` descriptors of the handle were replaced with the client's dma-buf. The
   vendor handle has two fds; the second one carries gralloc's own metadata, and the mapper
   reads the pixel format from it. Overwriting it made the format read back as
   `UNSPECIFIED (0)`. Only descriptor 0 is the pixels.

With both fixed, `IMPORT` succeeds inside SurfaceFlinger: the vendor mapper does accept a
dma-buf produced by mesa, as long as geometry, format and usage in the descriptor match what
the placeholder advertised.

Known limitation: this path makes the sfwm layer show **one** client, because the layer now
carries that client's buffer instead of a composited frame. It suits the
single-fullscreen-window case (Minecraft); leave `SFWM_SF_HOOK` unset for multi-window use.

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
