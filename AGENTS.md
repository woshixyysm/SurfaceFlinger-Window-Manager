# Agent guidance

This file defines the working rules for automated agents and contributors. Read it before
changing source, build files or documentation.

## Project model

sfwm is a native C++17 Wayland compositor and window manager for Android. It runs as a
Termux process. SurfaceFlinger owns the physical display and performs final composition.
sfwm creates SurfaceFlinger layers for Wayland surfaces and controls their geometry,
stacking and visibility through SurfaceComposerClient transactions.

Keep this architecture intact unless the user explicitly approves a redesign.

- The core is a Wayland server and window manager. Do not turn it into an Android Activity,
  a VNC server, a container runtime or a DRM/KMS compositor.
- The core binds the private SurfaceComposerClient ABI with dlopen and dlsym. Do not add a
  link-time dependency on libgui or vendor AOSP headers.
- The project builds as one translation unit. Include order in src/main.cpp is significant.
  Do not split modules into separate translation units without first resolving their shared
  static state.
- The core uses C++17. Check meson.build before changing language or compiler assumptions.
- The LSPosed bridge, KernelSU helper and service scripts are optional integrations. The core
  compositor must continue to start and present windows without them.

Agents may question an existing design when code or measurements provide a concrete reason.
Present the evidence and trade-offs first. Wait for approval before changing the project's
architecture or replacing an established backend.

## Read before editing

Start with the relevant source and documentation:

- README.md for the project model and current build.
- doc/usage.md for command-line options, environment variables and the control socket.
- doc/driver-notes.md for Android graphics, dma-buf, EGL and input behaviour.
- doc/bridge.md for LSPosed capabilities, configuration and wire protocol.
- doc/tiling.md for layout state and gestures.
- doc/nested-output.md for virtual-display investigation and its measured limits.
- docs/ONDEVICE.md for reference-device setup and measurements.
- android/ksu/README.md for the optional SurfaceFlinger import helper.

Treat source code as authoritative when it conflicts with documentation. Report the mismatch
and update the relevant document when the behavior changes. doc/nested-output.md includes
investigation and proposed routes. Check each route's recorded status before describing it
as implemented. Device-specific measurements in docs/ONDEVICE.md do not generalize to every
Android build.

Before editing, inspect the current branch, working tree, recent commits and the actual diff.
Trace the relevant function and its callers. Identify the intended behavior, the affected
files and the expected result. A commit title or a previous agent's summary is not a
substitute for reading the code.

## Change discipline

- Make the smallest change that fully addresses the stated problem.
- Keep unrelated rendering, input, service and documentation work out of the same patch.
- Avoid whole-file rewrites when a local edit is sufficient.
- Do not replace a file with an older snapshot to fix one function.
- Do not remove code because it looks unused until its callers, runtime selection and fallback
  paths have been checked.
- Do not rename interfaces, environment variables or command-line options as cleanup.
- Do not invent a new option or claim that an option exists without checking the parser.
- Do not add dependencies or move work into Java, Kotlin, an Activity or a helper process
  unless the task requires that change.
- Preserve useful comments that explain ABI assumptions, ordering constraints, failure modes
  and measured platform behaviour. Update comments when the implementation changes.

If the request is ambiguous, ask one focused question. If the requested fix would change
architecture or remove a working path, explain the consequence before editing.

## Critical implementation constraints

### SurfaceFlinger ABI and layer lifetime

- SurfaceComposerClient is a private Android ABI. Keep symbol lookup and handwritten ABI
  declarations isolated in src/compositor/sf_layer.hpp.
- On arm64, the non-trivial sp<T> return values used here travel through X8. Preserve the
  handwritten bridges and their register and stack conventions.
- Check every SurfaceControl before applying a transaction. Batch related layer changes so
  SurfaceFlinger does not display intermediate geometry.
- Hide an old layer before recreating it. Do not reuse a stale SurfaceControl or ASurfaceControl
  after its layer has been destroyed or replaced.
- Treat SurfaceFlinger crashes as system-level failures. Keep experimental paths opt-in and
  preserve a working fallback.

### Rendering and buffers

- The normal path imports client dma-bufs through EGLImage and composites with the GPU.
- SFWM_ZC uses the SurfaceFlinger layer buffer as the GPU render target. Preserve its format,
  stride, offset, modifier and fence handling.
- SFWM_ASYNC=1 uses the private ANativeWindow swap-interval entry point. It removes the
  BufferQueue producer wait. It does not change the panel scanout rate.
- SFWM_GL=0 selects the CPU copy path. Keep it usable as a fallback and comparison path.
- SFWM_DIRECT, SFWM_SF_HOOK and SFWM_PRESENT_BRIDGE are alternate presentation paths. They
  are opt-in. Keep the normal composition path available when they fail.
- The KernelSU SurfaceFlinger helper is experimental and intended for one fullscreen client.
  It must remain optional. Do not enable it by default for multi-window sessions.
- Wait for acquire fences before reading client data. DMA_BUF_IOCTL_SYNC handles CPU cache
  coherency; it does not replace the producer fence.
- Distinguish pixel stride from byte stride. Preserve per-plane offsets and modifiers.
- Do not key persistent buffer caches by file descriptor alone. Descriptors can be closed and
  reused. Include stable buffer identity such as inode and geometry where appropriate.
- Ensure destination pixels and alpha are initialized. An unwritten alpha channel can expose
  lower layers or create visible flashes.

### Input and geometry

- The default compositor startup does not take exclusive ownership of the touchscreen.
  --grab explicitly requests EVIOCGRAB. Keep that distinction.
- The KernelSU service configuration defaults to --z=1000000 without --grab.
- The current command-line parser has no --rotate option and does not auto-detect display
  rotation. The default output size is 1272x2800. Do not document automatic rotation or add
  rotation handling as an incidental input fix.
- Keep screen coordinates, logical buffer coordinates, zoom and scale factors explicit.
  Check the units used by hit testing, local-coordinate conversion, clamping and resize math.
- A touch gesture has a stable owner until release. Preserve contact identifiers and the
  correct down, motion and up sequence.
- Keep Android input reachable when the compositor is idle or when a hosted Android window
  owns the interaction. Avoid duplicate event delivery.

### Android bridge and optional modules

- The LSPosed bridge is optional. Its display and input-observation capabilities are disabled
  by default. Keep hook installation fail-open and guard hidden API calls.
- A failed hook, missing class or changed signature must leave the host process running.
  Never throw bridge errors into Android framework callbacks.
- InputObservationCapability can observe committed and composed text. Treat that data as
  sensitive. Do not log, persist or transmit input content beyond the bridge protocol's
  intended operation.
- Hosted Android windows follow Android window-manager stacking. System UI can cover them,
  and their z-order cannot be interleaved freely with Wayland layers.
- The pointer sprite host is used for hosted Android windows. Wayland clients use the
  compositor's cursor layer.
- The KernelSU service module only manages startup and runtime state. The SurfaceFlinger hook
  module is a separate optional acceleration path. Do not make either a core dependency.

### Tiling

- Tiling starts disabled.
- The tiler owns the geometry of managed windows and uses the normal configure path for
  Wayland clients.
- Keep the dwindle and master layouts, workspace ordering, floating exceptions and Android
  window integration consistent with doc/tiling.md.
- Do not introduce animations or per-window rules as part of an unrelated geometry fix.

## Build and verification

The compositor uses Meson and Ninja. Set CC=clang and CXX=clang++ explicitly on the reference
Termux build. The project targets C++17.

Run the structure check after source or include changes:

~~~sh
python3 tools/check_structure.py
~~~

CI also performs a cross-build against the Termux headers and libraries in deps/. Run the
available build checks when the environment supports them. Device-only behavior still needs
device testing, especially:

- SurfaceComposerClient symbol resolution and transaction lifetime.
- Touch input dispatch and Android input reachability.
- EGLImage imports, dma-buf modifiers and acquire/release fences.
- SFWM_ZC and SFWM_ASYNC behaviour.
- The optional bridge and KernelSU paths.

State exactly which checks were run. Never report a build or runtime test as successful when
it was not executed.

## Documentation and commit messages

Update documentation in the same change when user-visible behavior, options, defaults,
protocols or setup steps change. Keep README.md as the project overview. Put detailed command
and environment references in doc/usage.md. Keep device-only paths and measurements in
docs/ONDEVICE.md.

Write commit titles and session records in natural language or precise short sentences.
The title should describe the actual change. The body should explain the reason and scope.
Avoid generic fix messages, artificial verbosity, excessive em dashes and repeated
"not X but Y" constructions. Avoid hedging when the evidence is clear. State uncertainty
only when a detail has not been verified.

Before committing, inspect the staged diff and confirm that every changed file belongs to
the task. Do not claim unrelated files were preserved without checking their contents.

Do not rewrite published history or force-push unless the user explicitly authorizes it. When
history rewriting is authorized, inspect the branch head and parent chain, preserve unrelated
changes, use an expected-SHA lease and read the remote ref back after the update.
