# Command line

```
sfwm [options]
```

Options are matched by prefix, so they must be written as `--name=value` or as the exact
flag. Unknown arguments are ignored.

| option | default | meaning |
|---|---|---|
| `--size=WxH` | `1272x2800` | physical size of the output. SurfaceFlinger layer capacity follows it. |
| `--scale=N` | `3` | logical scale for the compositor's own UI and gestures. Clients are told the scale they render at through `wl_output` and fractional scale, not through this value. |
| `--z=N` | `1000` | base z-order for content layers. Cursor and input method layers are placed relative to it. |
| `--socket=NAME` | `sfwm-0` | Wayland socket name, created in `XDG_RUNTIME_DIR` and made mode 0666. |
| `--grab` | off | take the touch device with `EVIOCGRAB`, so Android does not also receive the same finger. |
| `--clickthrough` | off | layers stop accepting input. Touches reach the Android UI below instead. Can be toggled at run time with `ct`. |
| `--touch` | off | forward `wl_touch` to clients. Without it only pointer events are sent, because several toolkits handle `wl_touch` badly. |

The output size is not negotiated with Android. If it does not match the display, the
result is a smaller or larger layer that SurfaceFlinger scales, so set it from the real
panel resolution.

## Environment variables

| variable | default | meaning |
|---|---|---|
| `SFWM_GL` | unset | `0` selects the CPU copy path instead of GPU composition |
| `SFWM_GL_FLIP` | auto | vertical flip for the GPU path, when an imported buffer arrives inverted |
| `SFWM_GLDBG` | unset | log buffer geometry changes on the GPU path |
| `SFWM_TESTPAT` | `0` | draw the self-test pattern over every frame |
| `SFWM_TRACE` | `0` | write geometry, hit test and gesture records to `/data/local/tmp/sfwm-trace.log` |
| `SFWM_FRAMETIME` | unset | log per frame timing |
| `SFWM_NO_DEFER` | unset | read shm frames immediately instead of one commit later |
| `SFWM_DPI` | `168` | logical DPI reported through `wl_output`, clamped to 400 |
| `SFWM_IME_RESERVE` | `0` | reserve the bottom of the output for the keyboard |
| `XDG_RUNTIME_DIR` | `/data/local/tmp` | where the Wayland socket and the control socket are created |
| `XCURSOR_THEME`, `XCURSOR_PATH`, `XCURSOR_SIZE` | unset | cursor theme lookup |

## Control socket

`sfwm` opens several listeners for the same line protocol:

| endpoint | note |
|---|---|
| `$XDG_RUNTIME_DIR/sfwm-keys.sock` | mode 0666. Falls back to `/data/local/tmp` when the variable is unset |
| `/data/local/tmp/sfwm-keys.sock` | opened as well when `XDG_RUNTIME_DIR` points somewhere else, so a shell can reach it either way |
| `/data/data/<pkg>/files/sfwm-keys.sock` | one per input method host, owned by that application's uid with mode 0600. This is the only location an ordinary application can connect to, since it has the same label |
| abstract unix socket `@sfwm-keys` | no filesystem node, so no directory permissions involved |
| `127.0.0.1:5599` | loopback, for processes that hold the INTERNET permission |

The input method packages are tried by name and skipped when they are not installed.
Binding these endpoints is best effort: a listener that cannot be created is logged and the
others still work.

Every line is a command. A line that matches nothing is typed as text, so the socket can
be used as a keyboard without any prefix.

```
adb shell "echo 't hello' | nc -U /data/local/tmp/sfwm-keys.sock"
```

The same connection accepts an interactive session, which is what `sfwmc` does when it is
available.

### Text and keys

| command | meaning |
|---|---|
| `t <text>` or `text <text>` | type text. Delivered through `commit_string` when the focused client uses `text_input_v3`, otherwise through keysym injection |
| `k <evdev> [0\|1]` | raw evdev key code, pressed by default |
| `s <keysym>` | named keysym, for example `s Return`, `s Escape`, `s Left` |
| `pre <text>` | preedit string for an input method |
| `pre` | clear the preedit |
| `m <x> <y>` | pointer motion in screen coordinates, which also updates the focus |
| `c <x> <y> [0\|1]` | press and release at a point, or only one of the two |
| `p <x> <y>` | a full click at a point |

### Windows and gestures

| command | meaning |
|---|---|
| `zoom <z>` | apply a composer-side zoom to the focused window, clamped to 0.2 to 3.0, without sending a configure |
| `zoom` | print the zoom, buffer size and position of the focused window |
| `zoomend` | turn the current zoom into a configure, which makes the client redraw at the new size |
| `fit` | clamp the largest window into the output and centre it |
| `max` | ask the largest window to fill the output |
| `snap` | toggle snapping |
| `snap on\|off` | set snapping explicitly |
| `snap <dp>` | set the snap band in logical pixels, 0 to 200 |
| `ct` | toggle click-through |
| `ct on\|off` | set click-through explicitly |
| `dump` | print the compositor state: output, scale, gesture owner and every view |

### Desktops and hardware

| command | meaning |
|---|---|
| `ws <1..4>` | switch workspace |
| `ws` | print the workspace of every mapped view |
| `bright <0..100>` | set panel brightness |
| `bright` | print the current display state |
| `cct <K>` | set colour temperature through the platform white balance override |
| `cct off` | drop the override |
| `screen on\|off` | switch the panel |
| `screen` | print the current display state |
| `hide` | hide all layers and give the screen back to Android, while the process keeps running |
| `show` | restore the layers |
| `help` | print the short usage line |

### Input method channel

These are used by the input method host, not by hand:

| command | meaning |
|---|---|
| `ime-register` | register the calling connection as the input method host, which then receives `activate` and `deactivate` |
| `imeh <px>` | report the keyboard height in pixels |
| `ime on\|off` | raise or hide the keyboard |
| `imestate 0\|1` | report the keyboard state; `0` also makes clients leave text input focus |

## Typical session

```sh
# start, matching the panel resolution
./sfwm --size=1272x2800 --scale=3 --socket=sfwm-0 &

# a client connects to the socket
WAYLAND_DISPLAY=sfwm-0 foot &

# stop taking input so the Android UI can be used, then take it back
echo 'ct on'   | nc -U /data/local/tmp/sfwm-keys.sock
echo 'ct off'  | nc -U /data/local/tmp/sfwm-keys.sock

# bring a lost window back
echo 'fit'     | nc -U /data/local/tmp/sfwm-keys.sock
```

If the screen is needed for Android, `hide` releases it without killing the clients; `show`
brings the layers back at the same positions.
