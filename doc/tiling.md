# Tiling window management

sfwm keeps SurfaceFlinger as the compositor and adds a tiling layer on top, following the
user-facing behaviour of Hyprland: two layouts, directional focus, swap, a draggable ratio,
gaps, per-workspace order, and an explicit floating exception.

The tiler never resizes a surface. A tile is applied by moving the layer and asking the
client for the tile's logical size with an `xdg_toplevel.configure`, the same mechanism the
corner-drag resize already uses. The window becomes exactly the tile when the next buffer
arrives; until then it keeps its old size at the new position.

## Defaults and the free window behaviour

Tiling starts **off**. Without `tile on` the compositor behaves exactly as before: windows
are free-floating, moved by a long press, resized by a corner drag or a pinch, and snapped
to the edges. The tiling calls in `map()`, in the focus path and on a workspace switch all
return immediately while tiling is off, and the three-finger gesture branch requires tiling
to be on, so three fingers still reach the normal two-finger path.

Turning tiling off again clears the forced tile size of every window it managed and leaves
them where they are as ordinary floating windows.

## Model

- `g_tile_order` is the append-only order of tiled windows. It is the only layout state, so
  focus, swap and workspace changes never have to walk a tree.
- New windows join the order when they map. Windows that are unmapped, destroyed, or turned
  floating are pruned before any geometry is computed, so a deleted view is never read.
- One order serves all workspaces; the layout only assigns geometry to windows whose
  `ws` matches the active workspace.
- Panel/layer surfaces, the input method, the cursor, popups, subsurfaces, the session lock
  surface and the Xwayland root window are never tiled. Everything else is a candidate,
  including a full-output window such as a launcher or a desktop: tiling does not force it
  into the background the way the floating path does.
- Android windows hosted by the bridge module join the same order, after the Wayland ones.
  They are placed by moving their overlay window instead of by a configure, but they share
  the gaps, the split ratios and the resize rules.

## Layouts

**dwindle** (default): window `i` of `n` splits the remaining rectangle along its longer
side. The split belongs to the window that opens the group and carries its own ratio; with
no custom ratio it takes `1/(n-i)` of the space, which is the spiral behaviour Hyprland's
dwindle produces when windows are opened one after another.

**master**: the first `master` windows form a left column of width `ratio`, the rest stack
in the right column. `ratio` is clamped to `0.2 .. 0.85`.

Both layouts inset the screen by `gap` and keep the same gap between neighbours.

## Resizing a boundary, not a layout

Changing one window must not reflow the screen. A split stores its own ratio, so a resize
changes the boundary the focused window shares with its neighbour in that direction and
nothing else: windows on the other side of the tree keep their geometry, and no window
grows to fill a gap left behind.

The pinch gesture on a managed window drives the same path: finger travel is turned into a
ratio through the split's own extent, so the same movement means the same thing whatever
the window size. `tile even` forgets every custom ratio and returns to equal splits.

The master layout is the one exception by construction: the master column boundary is
global, so `tile resize` there changes `ratio`.

## Commands

Sent over the existing control socket (`$XDG_RUNTIME_DIR/sfwm-keys.sock`), one per line:

```
tile                        toggle tiling
tile on | off | toggle
tile layout dwindle|master
tile focus l|r|u|d          directional focus, also next|prev
tile swap l|r|u|d           swap with the neighbour in that direction
tile resize l|r|u|d [ratio] move that boundary (default 0.05, negative shrinks)
tile ratio <0.2..0.85>      master column share
tile master <n>             windows in the master column
tile gap <px>
tile even                   forget custom splits
tile float                  toggle the focused window between tiled and floating
tile fullscreen             toggle xdg fullscreen on the focused window
tile sync                   re-apply the layout now
```

`app` drives the Android windows that the bridge module hosts:

```
app open <slot> <package|home>   start an application (or the launcher) in that slot
app close <slot>                 release the window and its display
app list                         print what the module currently reports
```

Example:

```sh
echo 'tile on'                 | nc -U /data/local/tmp/sfwm-keys.sock
echo 'tile layout master'      | nc -U /data/local/tmp/sfwm-keys.sock
echo 'tile focus l'            | nc -U /data/local/tmp/sfwm-keys.sock
```

## Gestures

Three fingers are the window-management set **while tiling is on**; they never reach the
client. With tiling off the gesture set is inactive and three fingers behave as before.

| gesture | action |
| --- | --- |
| three-finger swipe left / right | move the focus to that neighbour |
| three-finger swipe up / down | swap with the neighbour above or below |
| three-finger tap | toggle the focused window between tiled and floating |

The swipe is evaluated on release against a threshold of 40 dp, so a resting finger cannot
move the focus. Two fingers on a managed window drag its boundary instead of scaling the
client, which is how a tile is resized from the touchscreen.

A managed window cannot be moved or resized freely: a long press moves the focus instead of
starting a move, a corner drag is not armed, and a client asking for `xdg_toplevel.move` is
focused without being moved. The layout owns the geometry, so nothing is left to fight over
it. With tiling off, or on a window that is floating, all of the free gestures are exactly
what they were.

## Focus, swap and stacking

Focus uses the centres of the current tiles: the window nearest in the requested direction
wins, with the cross-axis distance breaking ties, and the opposite direction is used as a
wrap-around fallback. Tiling focus follows the keyboard focus, so clicking a window or a
client requesting activation both update it. Focusing a tile also raises its layer, which
keeps the overlap during a transition predictable.

Swapping exchanges the two entries in the order and re-applies the layout. It swaps
positions, not sizes, so the window that was focused keeps its keyboard focus while it
moves.

## Interaction with the rest of the compositor

- The keyboard height and workspaces keep working: switching workspace re-applies the
  layout for the window set that just became visible.
- A window that is floating keeps the free-move, corner-resize and pinch gestures; the
  tiler ignores it until it is floated back.
- The input method, panels and the cursor stay outside the layout.
- Android windows live on their own virtual display and are drawn by the platform, so they
  are stacked above the Wayland layers; a tile of either kind keeps its rectangle, but the
  two kinds cannot interleave in z-order. Sizing is applied by moving that overlay window,
  which is why a tiled Android window keeps its density: the app gains space instead of
  being re-laid-out at a new DPI.

## Limitations

- No animations and no per-window rules; the layout is applied directly.
- `ratio` only affects the master layout; dwindle uses one ratio per split.
- Directional focus, swap and `resize` still address Wayland windows only; Android tiles
  take part in the geometry but not in the focus commands yet.
- A window that never answers a configure keeps its old size inside its tile.
