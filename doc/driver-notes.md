# Driver notes

What follows is about Android and Qualcomm/Adreno behaviour in general, as encountered
while building a compositor on top of SurfaceFlinger. Each point was reproduced on a
rooted arm64 device running Android 16, but the statement is about the platform, not about
one unit.

Things that only hold because of the machine this was developed on are deliberately left
out. That covers the compiler that happens to be first on `PATH`, the local Mesa tree with
its patches, the panel resolution and the names of the SELinux domains in use. Those are
recorded in the reference environment section of `README.md` and do not generalise.

## 1. There is no DRM device

The display path is KGSL, not DRM: `/dev/kgsl-3d0` exists and `/dev/dri/renderD128` does
not, and there is no card node to become DRM master of. The consequences are structural:

- no page flip, no atomic modeset, no plane configuration of your own;
- the wlroots DRM backend and everything built on it cannot drive the display;
- SurfaceFlinger owns the display, so a compositor either works through SurfaceFlinger or
  does not show anything at all.

A compositor in this position is a window manager that borrows another compositor's
output, which is the arrangement `sfwm` uses.

## 2. SurfaceFlinger as the presentation path

Layers are created through `SurfaceComposerClient` in `libgui`. Those symbols are not in
any public header, so they are bound with `dlsym` and the signatures are transcribed by
hand. On arm64 a function returning `sp<T>` returns through X8, so a call has to be
wrapped in a naked bridge that points X8 at the destination rather than pretending the
function returns a pointer in x0.

Pixels are written through the `ANativeWindow` that hangs off the `Surface` object. Two
rules follow from how a layer owns its buffer queue:

- allocate the layer once at the output size and crop with `setGeometry` instead of
  resizing the layer. `setBuffersGeometry` rebuilds the buffer queue, and during the
  rebuild SurfaceFlinger can go a frame without a buffer, which reads as a blank frame;
- do not create and destroy layers on every size change. Hiding and recreating per frame
  during a resize flickers the whole window.

Layer visibility and geometry are applied as transactions. Accumulating the changes of one
frame and applying them once avoids SurfaceFlinger compositing an intermediate state, which
is visible as a flicker when several layers move at the same time. A transaction carrying
an empty `SurfaceControl` makes `libgui` dereference null inside
`TransactionState::getLayerState()`, so every call has to check that the layer exists.

### Alpha and blending

Android 10 removed the `eLayerOpaque` layer flag; the blend mode now follows from the
buffer format. A layer created as `BGRA_8888` carries alpha, so whatever alpha the client
rendered with is blended:

- client pixels copied verbatim keep the alpha GL left behind, which is arbitrary and may
  change per frame, so the same content brightens and dims;
- pixels that were never written keep alpha 0, which SurfaceFlinger blends as "show the
  desktop through me", visible as a white flash on a black window.

Both are fixed on the writing side: write every pixel of the destination and force alpha to
`0xFF` while copying, or fill with opaque black. There is no flag that makes SurfaceFlinger
ignore alpha.

### Input occlusion

Since Android 12 a layer with `touchOcclusionMode` set to `BLOCK_UNTRUSTED` and sufficiently
opaque pixels swallows touches aimed at untrusted windows below it, even with an empty
`touchableRegion`. `setTrustedOverlay` and `setDropInputMode` are exported and return
success, but a process that is not the system uid does not get the mode changed, which is
easy to confirm with `dumpsys input`. The practical answer for decorative overlays such as
a cursor is to hide them when idle instead of relying on input flags.

## 3. Synchronising client buffers

A client commits a dmabuf before the GPU has finished writing it. `DMA_BUF_IOCTL_SYNC`
only flushes CPU caches; it does not wait for the write to finish, so a CPU read straight
after commit returns a mix of the current and the previous frame.

Android kernels export the implicit fence of a dma-buf with
`DMA_BUF_IOCTL_EXPORT_SYNC_FILE`, and `poll()` on the returned descriptor reports when the
write completed. Kernels without it answer `-ENOTTY`, so a CPU path needs a fallback that
accepts reading without synchronisation. The GPU path does not need any of this: sampling
an imported dmabuf makes the driver wait on the `dma_resv` fence by itself, which is a
reason to prefer it.

`wp_linux_drm_syncobj_v1` needs timeline syncobj support. `drmSyncobjCreate` returns
`EOPNOTSUPP` on drivers that do not implement it, and advertising the protocol anyway makes
clients wait on points that are never signalled. Probe the capability and advertise only
when it is present; clients fall back to implicit sync on their own.

## 4. Buffer formats

Only linear buffers can be read on the CPU, and only some tiled or compressed layouts can
be imported by EGL. Check the modifier before choosing a path and fall back rather than
assuming any dmabuf can be used.

Two unit traps in the Android APIs:

- `ANativeWindow_Buffer.stride` is a count of pixels, while the dmabuf stride is in bytes;
- the buffer offset inside the dmabuf is per plane, and a plane count of 1 does not mean
  the layout is linear.

A mapping cache for client buffers has to be keyed by inode, not by file descriptor.
Swapchain rebuilds close the old descriptor and the number is reused, so an fd-keyed cache
hands back a buffer that has already been released.

## 5. OpenGL ES and desktop GL

Android's own EGL offers OpenGL ES only. Desktop GL appears when the loaded EGL
implementation provides it, which in practice means a Mesa build that drives the GPU
through a freedreno backend. Nothing in the code depends on the version: the string
returned by `glGetString(GL_VERSION)` is logged once at startup and that is all the caller
sees.

Two consequences matter when writing the compositor:

- the GL version a client gets depends on which EGL implementation is found on the device,
  so a version requirement cannot be assumed, it has to be queried;
- a Mesa built with a KGSL gallium backend is what makes GL work at all on a device without
  DRM. Upstream freedreno ships an `msm` and a `virtio` DRM backend but no KGSL backend for
  the gallium driver, while turnip carries one for Vulkan. Supplying that backend is a
  prerequisite, not an optimisation.

### Getting the frame out of GL

If the EGL implementation has no `platform_android` support there is no `EGLSurface` for an
`ANativeWindow`, so `eglSwapBuffers` is not available. The workable arrangement is a
surfaceless context drawing into an FBO that matches the layer buffer, followed by
`glReadPixels` into the buffer returned by `ANativeWindow_lock`, with `GL_PACK_ROW_LENGTH`
carrying the SurfaceFlinger stride.

That read-back is one copy, but it removes three problems at once: the fence handling is
the driver's, the client stride and modifier are handled by EGL, and the destination is
written to completion before it is read, so no unpainted alpha survives. When an EGL
implementation with `platform_android` is available, the read-back can be replaced by
`eglCreateWindowSurface` plus `eglSwapBuffers` without touching anything above it.

## 6. Vulkan

Vulkan over KGSL is possible through turnip, which carries a KGSL backend. The proprietary
Vulkan driver is a different story: it expects gralloc allocations inside an application
process, and a process outside the app sandbox cannot allocate the buffers it requires.

For a compositor built on SurfaceFlinger this only matters insofar as clients render with
Vulkan and hand over dmabufs; the compositor never needs a Vulkan context of its own.

## 7. Hardware video decode

The decoder is exposed as a V4L2 memory-to-memory device. On Qualcomm's downstream vidc
driver the capture queue only accepts uncompressed formats in compressed (UBWC) memory as
dmabufs. Requesting a linear NV12 format makes `REQBUFS` or `QBUF` fail with `ENOMEM`
rather than with an error that names the real cause, which makes a V4L2 M2M client look
broken when it is asking for the wrong memory layout.

The consumer that works is the platform codec service: it negotiates gralloc buffers with
the graphics allocator and handles the layout itself. Reaching it from an ordinary process
means the codec HAL, which runs in a vendor process, and the gralloc mapper, which is a
vendor library, both have to be reachable, and that is only supported from inside the
application sandbox. A compositor running as root outside it is not in the sandbox.

The conclusion drawn here is to keep video decode out of the compositor: capture the
composed output with `screencap`, which uses the display capture path rather than a
per-application projection. On the device used for this work a MediaProjection session did
not deliver layers created outside an application process, while display capture did,
though this is worth verifying per device.

## 8. Input devices

There is no keyboard device on a phone: `/dev/input` holds the touch controller, the power
key and a few GPIO keys. Reading it requires root, and the device should be taken with
`EVIOCGRAB` while it is in use, or Android receives the same touches as well.

evdev key codes are not text. A keymap has no key codes for CJK input; that has to go
through `text_input_v3` and `commit_string`, which is why the compositor exposes both a key
injection path and a text path.

## 9. GPU compute

Adreno mobile parts report no double precision: `CL_DEVICE_DOUBLE_FP_CONFIG` is 0, no
`cl_khr_fp64` extension is listed, and Vulkan reports `shaderFloat64 = false`. Kernels and
shaders for these parts have to work in fp32 or emulate fp64 explicitly. Documentation
that mentions fp64 for an Adreno part generally refers to a different chip family, which is
worth checking before designing around it.

## 10. Handing descriptors to clients

A compositor outside the app sandbox passes descriptors to clients inside it. Whether that
works depends on MAC and on labelled networking, not only on the file mode: a descriptor
created by a privileged process can carry a label the receiving application is not allowed
to accept, and the denial is `dontaudit`, so nothing appears in the kernel log. The client
simply loses the descriptor, which surfaces much later as a broken keymap transfer or a
failed EGL initialisation.

Creating the descriptor in the client's own runtime directory avoids the problem, because
the label then matches the receiving domain. This is the reason the compositor creates the
keymap as a file in `XDG_RUNTIME_DIR` rather than a memfd.
