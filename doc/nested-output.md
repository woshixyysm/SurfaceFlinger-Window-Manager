# Nesting an Android display into the compositor

Today an Android window is an overlay of the default display: the module creates a virtual
display and shows its output surface in a `SurfaceView` window of its own process. That
works, and it is what makes an application tile possible at all, but it caps what the
compositor can do with it:

- **z-order is fixed.** The window belongs to the shell's window manager, not to sfwm. The
  shade, the control centre, the input method and floating small windows are system windows
  and are always above it; the compositor's own layers are always below it. Nothing can
  interleave an application tile with a Wayland window.
- **input has to be injected.** The compositor reads `/dev/input` itself, so a contact that
  lands in a hosted window has to be re-delivered into the hosted display.
- **the surface cannot be re-created cheaply.** Reordering those windows would destroy the
  surface the display renders into.

## What nesting means here

Instead of the shell hosting the display, the compositor hosts it: the virtual display
renders into a **SurfaceControl owned by sfwm**, and the module only launches the
application onto that display. The tile then *is* a compositor layer — placed, scaled,
raised, clipped and captured by the same code path as every Wayland window. The shell stops
drawing anything on the compositor's behalf, so the layer order question disappears.

Two properties make this the right model:

- A `VirtualDisplay` needs only an output `Surface`; who created it is irrelevant.
- `SurfaceControl` is transferable over binder: an `IGraphicBufferProducer` written into a
  `Parcel` and read on the other side becomes a `Surface` the receiver can use.

## Route A — the compositor creates the display through SurfaceFlinger

A virtual display does not need `DisplayManager` at all. SurfaceFlinger has its own display
API — the one the shell uses for `--new-display` style features:

```
SurfaceComposerClient::createVirtualDisplay(name, secure, policy, extra, refresh)
                                                     -> display token
Transaction::setDisplayLayerStack(token, stack)      -> which layers the display shows
Transaction::setDisplaySurface(token, surface)       -> where the display renders
Transaction::setDisplayProjection(token, rot, viewport, frame)
SurfaceComposerClient::destroyVirtualDisplay(token)
```

Permission is not the obstacle it looks like. AOSP has no `CREATE_VIRTUAL_DISPLAY`
permission; what is checked are the *flags* of a `DisplayManager` display (mirroring, secure,
trusted), and SurfaceFlinger's own access check accepts `AID_GRAPHICS`, `AID_SYSTEM`, or
`ACCESS_SURFACE_FLINGER`, while `PermissionCache` short-circuits `uid == 0`. The compositor
runs as root, so it passes. SELinux is the part to verify on the device: a KernelSU `su`
domain still needs `binder { call }` to `surfaceflinger`, which the existing transactions
already prove is allowed.

What is left for the module is only what needs Android policy: starting the application on
the display id, which it already does with `setLaunchDisplayId`.

Costs, in exchange for a real layer:

- the compositor owns display bookkeeping — density, layer stack, projection, teardown;
- the display is not a `DisplayManager` "trusted" display, so the shell may treat it as an
  external monitor (no system decorations unless asked for);
- release order matters: the display must go before its layer, or the surface stays owned by
  a dead display.

### What the device actually does (measured, Android 16 / PLQ110 / BP2A.250605.015)

The AIDL method is called `createVirtualDisplay`, not `createDisplay` — the latter no longer
exists in `ISurfaceComposer`, which has 81 transactions on this build:

| code | method | wire arguments |
| --- | --- | --- |
| 4 | `createVirtualDisplay` | `String name, boolean secure, byte OptimizationPolicy, String extra, float extra` → `IBinder` |
| 5 | `destroyVirtualDisplay` | `IBinder` |
| 13 | `getDynamicDisplayInfoFromId` | `long` |
| 14 | `getDynamicDisplayInfoFromToken` | `IBinder` |

`SurfaceComposerClient::createVirtualDisplay` is exported from `libgui.so`, so no transaction
is hand-written: the compositor calls it the same way it already calls
`createSurfaceChecked`. `service call SurfaceFlinger …` is useless for probing — SurfaceFlinger
answers every code with `Operation not permitted` because the shell command does not write the
AIDL interface token.

Measured, in this order, from a root process (`tools` probe, `nest_probe2.cpp`):

1. `createVirtualDisplay("sfwm-nest1", false, false, "", 0.0f)` succeeds: the log shows
   `Creating virtual display`, `processDisplayAdded`, `powerMode=On`.
2. The new display starts with `layerFilter={layerStack=4294967295}` — **no layer stack**. The
   caller has to supply one: `setDisplayLayerStack(token, 7)` (0 is the internal display, 5 and 6
   are the two hosted displays, so the next free value works).
3. A top-level `SurfaceControl` whose `setLayerStack` matches appears in that display's layer
   list and is composited with `Comp Type: CLIENT` and `usesClientComposition=true`.
4. `setDisplaySurface(token, producer)` — with both a plain `BufferQueue` producer and a
   `SurfaceControl`'s own producer — returns success and **nothing is ever queued**: the display
   composes into its client target and the frame is dropped. This build no longer implements the
   "a display renders into a caller-supplied surface" path.

So the display half of Route A works and the output half does not. `layerFilter` and the layer
list are reachable; the picture is not.

What was tried before concluding that, all with the display's stack already correct (7) and a
visible layer on it:

| variant | result |
| --- | --- |
| surface set first, stack later | accepted, no frames |
| stack first, surface later | accepted, no frames |
| surface + stack in one transaction | accepted, no frames |
| output layer's own `getIGraphicBufferProducer` | accepted, no frames |
| that layer moved off the display's stack (no feedback loop) | accepted, no frames |
| trailing `String` of `createVirtualDisplay` = an existing layer's name | accepted, no frames |

The frame counter of the output layer never moves and SurfaceFlinger prints no error, which
means it never even attempts the transfer. This build does not implement "a display renders
into a caller-supplied surface" for either a `BufferQueue` or a `SurfaceControl` producer.

That kills **Route B as well**: the module-hosts-the-display variant ends in the same
`setDisplaySurface` call, so handing our producer over a binder channel would change nothing.



## Route B — the module hosts the display, the compositor provides the surface

Keep display creation in the shell (it already works) and transfer the compositor's buffer
producer into it:

1. sfwm creates a `SurfaceControl` through its existing `SurfaceComposerClient` bridge and
   reads the `IGraphicBufferProducer` back (the same object `getSurface` already returns).
2. The producer is written into a `Parcel` and handed to the module over a binder channel.
3. The module builds `new Surface(producer)` and passes it to
   `DisplayManager.createVirtualDisplay`, keeping the current flag ladder and resize path.

This needs a binder channel between compositor and module. The module side already runs in
the shell process; the compositor side needs a `BBinder`/`BpBinder` pair the two processes
can name — a small service registered with servicemanager, or a socket that carries one
binder object once.

The cost is real: the compositor would carry `libbinder` plus a parcelable wrapper for the
producer, and both sides would have to agree on the ABI of that wrapper.

Measured, this route is dead for the same reason Route A is: it ends in the same
`setDisplaySurface` call, which this build accepts and ignores.

## Route C — capture the display instead of hosting its output

SurfaceFlinger will not hand a display's picture to a surface of ours, but it will hand the
picture to a **capture listener**. That is the path `screencap` and screen recording use, and
the one piece of display plumbing that is both exported and vendor-maintained:

```
ScreenshotClient::captureDisplay(DisplayId, CaptureArgs, sp<IScreenCaptureListener>)
BnScreenCaptureListener   -- exported, including its vtable
```

The compositor asks for one capture per frame of the hosted display, receives a
`GraphicBuffer`, wraps it as an `EGLImage` (`eglGetNativeClientBufferANDROID` +
`eglCreateImageKHR`) and composites it like any client buffer. No CPU readback, no
`SurfaceView`, no display ownership — the tile becomes an ordinary texture in the scene graph,
so z-order, scaling and clipping behave exactly like a Wayland window.

Costs and open points:

- one extra GPU composition pass per hosted display per frame, which is the price of reading
  a framebuffer back through the GPU instead of sharing a buffer;
- `captureDisplay` wants the 64-bit `DisplayId` from `dumpsys SurfaceFlinger --display-id`,
  not the `DisplayManager` id: one `dumpsys` fork per display creation, cached afterwards;
- the listener is a `BnScreenCaptureListener` whose single virtual is pure, so the compositor
  has to install one function in a copied vtable — the same hand-written ABI style as
  `sf_layer.hpp`, but it is the only way to receive frames without a second process.

A side effect worth stating: this also replaces the `screencap` subprocess in
`wlr-screencopy`, which today forks a process and reads pixels through a pipe.

## Ordering

1. Route A: answered — display creation works, the output surface does not.
2. Route B: dead for the same reason.
3. Route C is what is left, and it is the only one that also fixes screenshots.
4. The overlay path stays as the fallback. A compositor without the nested output must still
   show something, and every Android tile in this design is optional.


## The launcher

A launcher is the one application that is usually single-instance: launching `HOME` again
delivers the intent to the copy already on the default display, so a hosted launcher window
stays empty. Nesting does not change that, because it is a task-launch policy.

Two ways out, in order of cost:

- **Move the task instead of starting it.** `ActivityTaskManager.moveTaskToDisplay` on the
  launcher task puts the real launcher on the hosted display, keeping its state. The
  compositor already knows the display id, so this is one more command on the control
  channel and no new rendering path.
- **Give the launcher window its own compositor.** A small Wayland client that draws the
  application grid and starts applications is a normal tile, needs no display at all, and
  can show what the hosted displays contain by capturing their layers. This is the
  "partially nested" variant: the launcher becomes a compositor view of the session instead
  of an Android application.

The second is the one that looks like a desktop. It is also the one that does not depend on
what the vendor launcher allows.
