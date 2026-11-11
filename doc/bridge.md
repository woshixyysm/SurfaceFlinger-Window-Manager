# The sfwm bridge module

`android/` is an optional LSPosed module that lends sfwm two Android capabilities it cannot
have on its own: the height and the text stream of the input method, and additional virtual
outputs. sfwm neither links nor requires it: the module only publishes a line protocol, and
a compositor without a reader simply sees one output and no keyboard notifications.

The module is written against framework classes only, so the same code is expected to run
on AOSP-derived shells; the portability notes at the end say what is and is not assumed.

## Process model

Three pieces, each with one job:

- `SfwmBridge` is the Xposed entry point named in `META-INF/xposed/java_init.list`. It reads
  the configuration, applies the kill switch, and hands the process to the one capability
  that answers for its package. It contains no Android policy.
- `Capability` is the extension contract: which package it covers, whether the configuration
  enables it, and how it is installed. Adding or removing a capability never touches the
  entry point.
- `BridgeReport` is the single model of what the bridge currently provides, and `BridgeLink`
  is the single way it leaves the process. Both capabilities publish through them, so the
  wire format cannot drift between them.

Every installation step is guarded: a missing class, a changed signature or a refused
window is logged and leaves the host process untouched. Hooks never throw into Android.

Current capabilities:

| capability | covers | configuration |
| --- | --- | --- |
| `SystemUiDisplayCapability` | the SystemUI process | `display=0` (default) |
| `InputObservationCapability` | every scoped process except SystemUI | `ime=0` (default) |

`scope.list` decides which processes may load the module at all. It is the only place that
enumerates packages; the code itself only special-cases the shell process, because that is
where the outputs are hosted.

## Configuration

`/data/adb/sfwm-bridge/config`, one `key=value` per line, `#` starts a comment:

```ini
ime=0            # input observation
display=0        # virtual outputs
channel=both     # socket | file | both | none
outputs=1        # 0..4
output.0.width=636
output.0.height=1400
output.0.dpi=560
output.0.x=612
output.0.y=1400
```

Both capabilities start disabled. An output window with nothing rendering into its layer
stack is a black rectangle on top of the shell, so `display` only makes sense once a
compositor reads the protocol; enabling it before that is purely a test.

`/data/adb/sfwm-bridge/disable` disables everything. Both paths fail open: an unreadable
path means "not disabled" and "defaults", never "switch the input method off", so a SELinux
denial cannot leave the device without a working keyboard.

An output without an explicit `output.N.*` block is derived from the physical panel (lower
right quadrant, panel density) and follows panel changes. An explicit block is honoured
verbatim and does not move when the panel rotates, so a configured layout is never silently
rewritten.

## Wire protocol

A snapshot describes the whole current state; it is published once per change, over the
socket in one connection, or written atomically to a state file:

```
v 1
ime <0|1> <height>
lock <0|1> <panelHeight>
display <index> <displayId> <layerStack> <w> <h> <dpi> <x> <y> <package>
```

`lock 1` means the Android keyguard or a dark screen owns the display: the module hides its
windows and the compositor hides its own layers, so nothing of the session shows above the
lock screen. Nothing is released — the applications and their displays keep running.

The last field is `-` for a plain output, `home` for a window hosting the shell, and a
package name otherwise. A reader that only knows the geometry can stop before it.

Events are deltas and are never part of a snapshot:

```
t <text>        committed text, one line per protocol line
pre <text>      preedit; a bare pre clears it
s <keysym>      Home, End, Escape, BackSpace, Return, Tab, Up, Down, Left, Right
```

Text cannot carry a newline inside a `t` command, so a line break is encoded as `s Return`
between the two halves.

Channels, in order: the abstract socket `sfwm-bridge`, then `/data/local/tmp/sfwm-bridge.sock`,
then the process's own files directory (`<files>/sfwm-bridge.state`) as a snapshot-only
fallback. A hooked process may write only inside its own data directory and cannot create a
file in `/data/local/tmp`, so those two routes are the only ones that need no extra
permission. Events cannot be replayed from a file, so `channel=file` mirrors nothing and
typing still behaves normally inside Android.

## Application windows

An output can host an application instead of showing an empty layer stack. The compositor
owns the layout, so it also decides where the window sits; the module owns only what the
platform needs to draw it.

The compositor drives it over an abstract socket named `sfwm-bridge-ctl`, one command per
line, several per connection:

```
place <slot> <x> <y> <w> <h>   move and resize the window, density unchanged
open  <slot> <package|home>    start an application on that slot's display
close <slot>                   release the window and its display
touch <slot> <phase> <x> <y>   forward one contact, in the display's own pixels
status                         republish the snapshot
```

Touch is forwarded, not relayed, and the window decides. A contact inside a hosted window is
the window's, so whatever the shell put above it — the shade, the control centre, the input
method, a floating small window — takes the contact first and the application never sees it.
The module forwards from a touch listener on the window, in the display's own pixels, with
`InputManager.injectInputEvent` after `MotionEvent.setDisplayId` (hidden, called
reflectively). The `touch` command still exists for tooling, but the interactive path does
not use it: the compositor cannot know what is drawn above a hosted window.

Layer order is fixed by construction. A hosted window is an overlay of the default display,
so the shade, the control centre, the input method and floating small windows — all system
windows — are above it, and cannot be reordered without recreating the surface the display
renders into. The layout keeps the hosted windows from overlapping, which is what makes the
fixed order harmless; a window that should really interleave with the Wayland layers would
have to be composited by the compositor itself instead of hosted by the shell.

A placement resizes the virtual display and moves the overlay window, in that order, so the
application never sees a window larger than its display. The density is pinned to the
panel's, which is what makes a tiled window behave like a tiled Wayland window: growing it
gives the app more pixels instead of a different layout. Everything runs on the main thread,
because the windows were created there and a view touched from the socket thread throws.

Starting an activity on a display that is not the default one needs
`ActivityOptions.setLaunchDisplayId`, which is hidden; it is called reflectively and the
plain `startActivity` is the fallback. `home` starts `ACTION_MAIN`/`CATEGORY_HOME` instead of
a package, which is how the launcher becomes an ordinary window. A display is started once
per request, so resizing never relaunches the app.

A launcher is usually single-instance: the platform then delivers the intent to the copy
already running on the default display and the hosted window stays empty. Ordinary packages
do start on a secondary display, and `close` releases the window if a package cannot be
hosted.

`home` therefore takes a different path: the shell's own home root task is **moved** onto the
slot's display with `ActivityTaskManager.moveRootTaskToDisplay`, so the tile shows the real
launcher and keeps its state. The task id comes from `getAllRootTaskInfos()` (activity type
home), with `dumpsys activity activities` as the fallback, and `am display move-stack` as the
fallback for the move itself. The move is refused once the platform already owns a home root
task on the default display ("addRootTaskReferenceIfNeeded … already exist"), and then the
launch path below it runs instead; on this build that path does produce a launcher task on the
hosted display, so a tile appears either way.

The same slot can be configured up front with `output.<n>.package=` (`home` or a package) and
`outputs=<n>`; a slot with no package stays a plain output.

## Input observation

Three framework observation points, all in the input method's own process:

- `InputMethodService.getCurrentInputConnection` returns the connection the keyboard writes
  through. The module wraps it (`KeystrokeMirror`), reports what is committed, composed or
  pressed, and forwards every call unchanged. The wrapper is cached per source connection so
  repeated calls do not stack wrappers.
- `InputMethodService.onComputeInsets` receives the insets the service computed. The keyboard
  height is `panelHeight - contentTopInsets`. The field is read reflectively because it is
  hidden, `top` is the fallback, and a value outside `(0, panelHeight)` is discarded as a
  transient. `panelHeight` comes from the system resources, not from the candidate view.
- `InputMethodService.onStartInputView` / `onFinishInputView` mark the keyboard up and down.
  They are lifecycle callbacks, which is why no window operation is watched.

`BaseInputConnection` is hooked separately (`EditorMirror`) for editors that receive text
without going through an input method. It is an independent observation point, not a
refinement: a terminal has no input method connection to wrap.

State is published only on transitions (`BridgeReport.markKeyboard` returns false when
nothing changed), so a keyboard animation produces at most a few frames of messages rather
than one per inset computation.

## Virtual outputs

An output is three objects, created in this order:

1. `OutputStage` adds one overlay window per slot, with a `SurfaceView` inside it. It is
   translucent, hardware accelerated, not focusable and not touch modal: sfwm reads
   `/dev/input` itself and routes events, so the window only has to stay out of the way.
2. The surface callback hands the surface to `OutputSurfaceFactory`, which creates the
   virtual display through `DisplayManager` and remembers the size it was created with.
3. `VirtualOutputs` reads the display id and layer stack, records them in `BridgeReport`,
   and publishes.

The mount table is what keeps outputs stable. A placement that only moves the window does
not resize or recreate the display, so the layer stack — and every window sfwm has put on
it — survives. A size change calls `resize`, which keeps the display id; recreation happens
only when the platform refuses the resize.

Creation tries the trusted flag first (`VIRTUAL_DISPLAY_FLAG_TRUSTED`, value 256, hidden
from the SDK), then the public combinations, because a trusted public own-content display is
the one that behaves like a monitor for the shell. A refused candidate is logged with its
cause and the next one is tried.

`Display.getLayerStack()` is hidden and read reflectively; when it cannot be read the display
id is used, which is the same value for every display the platform creates by itself.

Panel changes (`ACTION_CONFIGURATION_CHANGED`) re-read the panel and re-present the existing
surfaces. Explicit placements are untouched; defaults follow the new panel. The receiver is
registered for the process lifetime and unregistered on `stop()`.

## Portability

What the module assumes, and what happens when the assumption does not hold:

- **Startup points**: `com.android.systemui.SystemUIApplication#onCreate` and the component
  factory's `createSystemUIApplication`. Both exist in AOSP; vendor shells that move the
  application class usually keep one of them. If neither is found, the display capability
  logs and does nothing.
- **Overlay permission**: `TYPE_APPLICATION_OVERLAY` needs `SYSTEM_ALERT_WINDOW`, which the
  shell holds in AOSP. Without it `addView` throws and the slot is reported as not mounted.
- **Hidden APIs**: two are used, both behind `runCatching` with a documented fallback:
  `Display.getLayerStack()` and the `contentTopInsets` field. Hidden-API enforcement does not
  apply to the shell or to the input method process on AOSP, and both reads fail soft.
- **Input method**: only framework classes are hooked, so the code does not depend on which
  keyboard is installed. Which processes are observed is decided by `scope.list`; changing
  the keyboard means editing that list, not the code.
- **Virtual display flags**: `VIRTUAL_DISPLAY_FLAG_TRUSTED` is not in the public SDK and may
  be rejected; the flag ladder then falls back to the public forms.
- **Config and kill switch**: `/data/adb/sfwm-bridge/` is the usual module-owned path. When
  the path is unreadable the module keeps its defaults rather than disabling itself.
- **No vendor classes**: nothing here resolves an OEM class name, so a shell update can at
  worst remove a hook point, which is logged and degrades to "capability unavailable".

## Failure behaviour

| failure | result |
| --- | --- |
| configuration unreadable | defaults, module stays enabled |
| kill switch present | no capability is installed |
| startup class missing | display capability logs and does nothing |
| window rejected | that slot stays unmounted, others continue |
| display creation refused | flag ladder continues, then the slot reports no display |
| socket unreachable | snapshot goes to the state file; events are dropped and logged at most every 10 s |
| hook signature changed | the call proceeds unchanged, the message is logged once |
