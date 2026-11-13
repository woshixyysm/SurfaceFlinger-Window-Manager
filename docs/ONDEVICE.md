# Running sfwm on the device

Everything here was learned the hard way, one session at a time. It is written down so a fresh
session does not have to rediscover it.

Device: OnePlus PLQ110, Android 16, Adreno 830, KernelSU, SELinux **Enforcing**.

---

## 1. The shape of the thing

```
  Minecraft (Java, Termux)  --wayland-->  sfwm  --SurfaceControl-->  SurfaceFlinger
                                            |
                                            +-- writes straight into the layer's dmabuf
```

sfwm is a Wayland compositor running as an unprivileged Termux process. It creates its own
layer with `SurfaceComposerClient::createSurfaceChecked` and composites Wayland clients directly
into that layer's buffer. Nothing is copied through the CPU on the fast path.

Two directions that are **not** used, and why:

- **VirtualDisplay** (the LSPosed module's original design) — backwards; it makes the shell own
  the frames instead of letting the compositor write into a layer.
- **Passing dma-bufs to an app-owned Surface** (`present_host.cpp`, `PresentActivity`,
  `SFWM_PRESENT_BRIDGE`) — this works but was unnecessary: it duplicates a path that already
  existed. Kept dormant as a fallback in case a vendor ever breaks the swap interval call.

---

## 2. Why it used to be stuck at 60 fps — and the one call that fixed it

The old zero-copy path was:

```
ANativeWindow_dequeueBuffer -> render into that buffer's dmabuf -> queueBuffer
```

`dequeueBuffer` on a BLAST buffer queue is paced by the display refresh. That single call was
the frame rate ceiling. It was proven with throwaway clients: a GL client that only calls
`glClear` still measured 59.8 fps, and a pure shm Wayland client released a buffer every
47.9 ms ≈ 3 × 16 ms.

The fix is the same thing FCL / PojavLauncher do — there is no secret API:

```c
/* the private ANativeWindow vtable; equivalently android::Surface::setSwapInterval(0) */
window->setSwapInterval(window, 0);
```

`setSwapInterval(0)` puts the BufferQueue into **async mode**: the producer no longer waits for
a free buffer, so `eglSwapBuffers` / `dequeueBuffer` return immediately. SurfaceFlinger keeps
sampling whatever the newest buffer is at scanout.

Implementation: `window_set_swap_interval()` in
[`src/compositor/sf_layer.hpp`](../src/compositor/sf_layer.hpp). It reaches the private
`ANativeWindow` layout (magic `0x5f776e64`, `'_wnd'`) and calls the `setSwapInterval` slot.
Newer Android versions append fields, so only the magic is validated — the slot sits before
everything that was ever added.

Enabled with `SFWM_ASYNC=1`. On any failure it prints why and the old paced path takes over,
so it is a hybrid by construction.

### What it does and does not change

- It raises the **submitted** frame count (the F3 number, and Sodium's frame-time histogram).
- It does **not** raise the physical scanout rate. The panel still scans at its own rate.
- It does remove the vsync stall, so input latency drops and the render loop runs free.

---

## 3. Device environment

| thing | value |
| --- | --- |
| sfwm binary | `/data/data/com.termux/files/home/lg/sfwm/sfwm` |
| built from | `/data/data/com.termux/files/home/sfwm-repo` (`meson` + `ninja -C build`) |
| Termux uid | `10391` |
| runtime dir | `/data/local/tmp/sfwm-xdg` (`wayland-0` symlink lives here) |
| sfwm log | `/data/data/com.termux/files/home/sfwm-test.log` |
| MC log | `/data/local/tmp/mc-run.log` |
| key injection | `/data/local/tmp/sfwm-keys.sock` |
| Minecraft | `~/.minecraft/versions/1.21.11-Fabric 0.19.1`, Sodium `0.8.14+mc1.21.11` |

### Build and deploy

```sh
# on device, as the Termux uid
su 10391 -c 'sh /data/local/tmp/bsfwm.sh'
# which is: cd ~/sfwm-repo && ninja -C build
```

After a rebuild the binary has to be copied and re-labelled, or SurfaceFlinger refuses the
binder calls:

```sh
cp ~/sfwm-repo/build/sfwm ~/lg/sfwm/sfwm
chown 10391:10391 ~/lg/sfwm/sfwm; chmod 755 ~/lg/sfwm/sfwm
$PREFIX/bin/ctxfix -u 10391 --mode 755 ~/lg/sfwm/sfwm
```

`ctxfix` exists for exactly this: a binary copied into Termux's data dir gets the wrong SELinux
label, and without it the `SurfaceComposerClient` calls fail with `EPERM`.

### LD_PRELOAD chain

```
/data/local/tmp/libeglcompat.so        # EGL compatibility shim, must be first
/data/local/tmp/libfdshim.so           # lets a Termux process pass fds to system services
/system/lib64/libnativewindow.so
/system/lib64/libbinder_ndk.so
/system/lib64/liblzma.so
```

### Environment variables

| name | meaning |
| --- | --- |
| `SFWM_ZC=1` | zero-copy: composite into the layer's dmabuf instead of reading pixels back |
| `SFWM_ASYNC=1` | **the important one** — async BufferQueue, removes the vsync ceiling |
| `SFWM_FPS=N` | layer frame-rate vote, and the pacing cap for the hook path (default 120) |
| `SFWM_FRAMETIME=1` | print a `[perf]` line every 60 frames |
| `SFWM_REFRESH=1` | keep re-voting the layer frame rate |
| `SFWM_LIBEGL` | which libEGL to load |
| `SFWM_DRM_DEVICE` | `/dev/dri/renderD128` |
| `SFWM_SF_HOOK` | KSU helper path, needs the injected `.so`; **not** used normally |
| `MESA_LOADER_DRIVER_OVERRIDE=msm` | Turnip |
| `LIBGL_DRIVERS_PATH` | `$PREFIX/lib/dri` |

---

## 4. `sfwmctl` — use this instead of shell scripts

`/data/local/tmp/sfwmctl`, source in [`tools/sfwmctl.cpp`](../tools/sfwmctl.cpp). Every path and
variable above is baked into it so no session has to re-derive them.

```sh
sfwmctl up                    # kill, deploy, start with zc + async
sfwmctl up --fps 300          # raise the layer frame-rate vote
sfwmctl up --scale 2          # client renders at 2x, layer shows it 1:1
sfwmctl up --sync             # A/B reference: the old vsync-paced path
sfwmctl mc                    # launch Minecraft, wait for the world, press F11
sfwmctl key F3                # toggle the debug screen
sfwmctl status                # pids, SELinux, refresh rate, last [perf] lines
sfwmctl shot /data/local/tmp/a.png
sfwmctl stop
```

Build it with the NDK:

```powershell
clang++ --target=aarch64-linux-android26 -O2 -std=c++17 -o sfwmctl tools/sfwmctl.cpp
adb push sfwmctl /data/local/tmp/sfwmctl
```

---

## 5. Sockets and wire formats

| socket | owner | purpose |
| --- | --- | --- |
| `$XDG_RUNTIME_DIR/sfwm-0` | sfwm | the Wayland display |
| `/data/local/tmp/sfwm-keys.sock` | sfwm | key injection, plain text: `s F3\n` |
| `@sfwm-sf` (abstract) | KSU helper inside SurfaceFlinger | fixed 28-byte request frames |
| `@sfwm-out` (abstract) | `present_host.cpp` | unused fallback, 20-byte header + one fd |

`@sfwm-sf` is abstract rather than a file because `/data/adb` is `0700 root` and
SurfaceFlinger runs as uid 1000.

---

## 6. Measured

| configuration | MC F3 |
| --- | --- |
| vsync-paced, 60 Hz panel | 59–61 |
| vsync-paced, 165 Hz panel | 55 (frame time crosses three vsyncs) |
| vsync-paced, 165 Hz, occasional | 77 |
| **async, 165 Hz, full 1272×2800** | **207** (p50 = 216) |
| **async, `--scale 3`, client renders 424×933** | **396** |

Composite cost at full resolution with `SFWM_ZC=1`: **2–3 ms**.

Panel refresh is verified with:

```sh
dumpsys SurfaceFlinger | grep -m1 -o 'vsyncRate[= ][0-9.]*'
```

`cmd display set-user-preferred-display-mode` and `settings put system min_refresh_rate` are
unreliable on this device. The 165 Hz that sticks comes from the layer's own
`txSetFrameRate` vote (`[sf] layer frame rate -> 165.0 Hz`).

---

## 7. Traps that cost real time

- **`screencap` inherits `LD_PRELOAD`.** In a shell that exported it, `screencap` dies with
  `cannot locate symbol "eglDestroySyncKHR"`. Always run it in a clean shell (or `sfwmctl shot`).
- **The green number in the corner is Android's refresh-rate overlay**, not Minecraft's FPS.
  Press F3 for the real counter.
- **Two Minecraft windows** used to appear because a layer recreate did not `hide()` the old
  `SurfaceControl`; the ghost layer kept being composited every frame.
- **A cached `ASurfaceControl` across a layer recreate** pushes a transaction at a destroyed
  control and kills SurfaceFlinger with `SIGSEGV` on a binder thread.
- **`ENOSYS` (-38) from `dequeueBuffer`** means the cached `Surface` went stale after a resize.
  The layer has to be rebuilt.
- **Full resolution + `BGRA_8888` cannot always be imported** into SurfaceFlinger by the vendor
  mapper (`SnapAlloc: Error while getting constraints from display libs`). Only matters for the
  `SFWM_SF_HOOK` path; the zero-copy path never imports.
- **`dlsym(RTLD_DEFAULT, "ANativeWindow_dequeueBuffer")` works**, but going through
  `libnativewindow.so` explicitly answers `ENOSYS`. `nm` is not available on this device, which
  made this look like the symbol was missing.
- **Unbounded `ASurfaceTransaction_setBuffer`** floods and crashes SurfaceFlinger. The
  `SFWM_SF_HOOK` path therefore needs pacing; the zero-copy path does not.
- **`--scale=N` scales the client buffer up**, it does not make the client render fewer pixels.
- **`sfwm --help` is not a thing.** Any unknown argument just starts another compositor.

---

## 8. Health check

```sh
sfwmctl status
```

A healthy session looks like:

```
sfwm:            pid ... up 08:43
surfaceflinger:  3631
selinux:         Enforcing
refresh:         vsyncRate=165.00
[perf] zc 1272x2800 -> sf 1272x2800 composite 3 ms
```

Then `sfwmctl key F3` and read the top-left of the screen.

---

## 9. Running it as a service (optional)

`android/ksu-service/` installs as a KSU module and exposes `sfwm`, `sfwm-service` and `sfwmctl`
in `/system/bin`. Disable or remove the module and nothing is left behind.

```sh
sfwm-service start | stop | restart | status | config
```

**Where state lives.** Not in `/data/data/com.termux` — PackageManagerService has wiped that
directory on this device more than once. Not in `/data/adb` either: it is `0700 root`, so the
Termux uid cannot even traverse it, and paths under it land in `mountinfo`, which is exactly
what a mount-marker scan looks for. The runtime is `/dev/sfwm`: tmpfs, not a mount point, not
under `/data`, and unreachable by any app-data cleanup.

Directory mode must be **0777**. `0711` silently prevents the Wayland socket from appearing.
The socket itself is then `0770`, owned by the Termux uid.

Config in `/data/adb/modules/sfwm-service/config.sh`: scale, fps, async, cursor, cursor theme
and size, idle exit, log size.

### Cursor

- `SFWM_CURSOR=0` hides it outright, whatever the client asks for.
- `sfwmctl cursor off|on` flips it live. The compositor polls `<runtime>/cursor-off` every 32
  frames, because neither the environment nor a socket command can change a running process.
- Theme and size come from `XCURSOR_THEME`, `XCURSOR_SIZE` and `XCURSOR_PATH`.

---

## 10. One more trap

- **`sh "$RUN/env"` exports into a child shell.** The compositor inherited nothing, so it never
  saw `XDG_RUNTIME_DIR`, `LD_PRELOAD` or the Mesa variables, and quietly created no Wayland
  socket where anyone expected it. Use `. "$RUN/env"`. This cost an entire debugging round: every
  symptom pointed at SELinux and the directory mode, both of which were innocent.