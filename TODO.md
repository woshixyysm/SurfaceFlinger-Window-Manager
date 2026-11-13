# TODO

Development notes for known defects, validation work and future directions. Items are
intentionally grouped by status; this is a working roadmap, not a release schedule.

## Bugs

- [ ] **sfwm layers flicker when Android activities update.** The reported symptom is that
  layers drawn by sfwm flicker when an Android activity updates underneath them. Reproduce
  the exact transition and trace SurfaceFlinger layer state, visibility, z-order and transaction
  timing before choosing a fix. Root cause is not established.
- [ ] **Hosted Android window lifecycle.** Verify that activity and virtual-display teardown,
  recreation and relaunch leave no stale overlay windows, slots or layer state.

## Device validation

- [ ] Test a physical keyboard and pointer through the replacement USB-C dock.
- [ ] Exercise Wayland and hosted Android windows together under floating and tiled layouts.
- [ ] Record repeatable frame-time, input-latency and buffer-synchronization measurements
  across the normal GPU path and available fallback paths.
- [ ] Add focused regression checks for layer create/destroy, resize, visibility and rapid
  Android activity updates.

## Planned

- [ ] **Multiple physical displays.** Add output discovery and per-display state, then support
  independent resolution, scale, layout placement, client assignment and display lifecycle
  changes. Define how SurfaceFlinger display ownership and output routing map to Wayland
  outputs before implementing the manager.
- [ ] **Hardware-composer path evaluation.** Determine when SurfaceFlinger can promote sfwm
  layers or client buffers to HWC overlay planes. Validate format, usage, dataspace, crop,
  transform, synchronization fences and composition decisions on target devices. Keep the
  GPU composition path as the known baseline and measure any overlay path against it.
- [ ] **Android bridge lifecycle.** Make hosted activity creation, updates, focus, close and
  virtual-display release predictable during workspace changes and compositor shutdown.
- [ ] **Control interface review.** Separate stable runtime controls from temporary device
  workflows. Keep the compositor's command protocol small and documented; consider moving
  frequently used runtime operations behind a dedicated client only after the protocol is
  settled.

## Investigation

- [ ] **sfwmctl scope.** The current `tools/sfwmctl.cpp` is a device-specific deployment and
  test helper with hard-coded paths, preload libraries and a Minecraft workflow. Decide which
  operations are generally useful before turning it into a project-wide CLI. Do not treat its
  current command set as the compositor's stable public interface.
- [ ] **Composition ownership and terminology.** Document the distinction between sfwm's
  EGL composition into a SurfaceFlinger layer, SurfaceFlinger's own composition policy and
  HWC plane assignment. Avoid describing the existing GPU path as direct hardware overlay
  composition.
- [ ] **Output architecture.** Review the single-output assumptions in geometry, protocol
  advertisement, capture, cursor placement, input mapping and Android bridge state before
  starting multi-display implementation.

## Scope notes

- The compositor core must remain usable without LSPosed, KernelSU or the service module.
- The SurfaceFlinger ABI boundary stays isolated from the Wayland and rendering layers.
- Experimental acceleration paths must retain a tested fallback.
- A task moves to complete only after its behaviour is implemented and the relevant device
  validation has been recorded.
