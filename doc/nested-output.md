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
SurfaceComposerClient::createDisplay(name, secure)   -> display token + layer
Transaction::setDisplaySurface(token, surface)       -> where the display renders
Transaction::setDisplayProjection / setDisplayLayerStack
```

The compositor already owns `SurfaceComposerClient` and a `SurfaceControl`, so both ends of
that call are in one process: the display renders into a layer the compositor owns, which is
exactly the nesting this document is about, with no surface crossing a process boundary and
no parcelable ABI to agree on.

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

## Ordering

1. Route A first: it is a single binder call and answers the only open question (is a
   root-owned virtual display accepted). If it is, the rest is display bookkeeping.
2. Route B only if A is refused, because it is the expensive one.
3. The overlay path stays as the fallback. A compositor without the nested output must still
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
