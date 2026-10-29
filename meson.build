# sfwm - a Wayland compositor that uses SurfaceFlinger as its compositor
#
# Four build rules are required here:
#   1. name clang++ explicitly; a stock Termux points c++ at clang, but a local GCC
#      toolchain ahead of it on PATH rejects the designated initialisers below
#   2. take only the libraries from pkg-config, never its cflags, or
#      -I$PREFIX/include shadows the bionic sys/cdefs.h
#   3. compile the generated protocol .c files as C into the executable, since in C++
#      wl_interface would become internal linkage
#   4. cpp_std=c++17 is required, the source uses out of order designated initialisers
project('sfwm', ['c', 'cpp'],
  default_options: ['cpp_std=c++17', 'buildtype=release', 'warning_level=0'])

prefix = get_option('prefix')
gen    = find_program('sh')
wp_dir = join_paths(prefix, 'share/wayland-protocols')

add_project_arguments(
  '-g',
  '-idirafter', join_paths(prefix, 'include'),
  '-idirafter', join_paths(prefix, 'include/libdrm'),
  language: ['c', 'cpp'])

libdir  = join_paths(prefix, 'lib')

# A local GCC toolchain installs its runtime outside lib/, so the directory is used when it
# exists and skipped otherwise. Stock Termux does not need it.
fs      = import('fs')
gccdir  = join_paths(prefix, 'opt/gcc-17/lib/gcc/aarch64-linux-android/17.0.0')

# Modules include each other with paths relative to src/, such as "core/config.hpp".
# Such an include searches the including file's directory first, so src is on -I.
inc = include_directories('src')
ld_extra = [
  '-L' + libdir,
  '-lwayland-server', '-ldrm', '-lxkbcommon', '-landroid', '-llog',
  '-static-libgcc', '-Wl,-rpath,' + libdir,
]
if fs.is_dir(gccdir)
  ld_extra += ['-L' + gccdir]
endif

# System protocols come from share/wayland-protocols, local ones from protocols/
proto_entries = [
  ['stable/xdg-shell/xdg-shell.xml', 'xdg-shell', '', true],
  ['stable/linux-dmabuf/linux-dmabuf-v1.xml', 'linux-dmabuf-v1', '', true],
  ['protocols/wayland-drm.xml', 'wayland-drm', '', false],
  ['protocols/wlr-layer-shell-unstable-v1.xml', 'wlr-layer-shell-unstable-v1', 'ns', false],
  ['protocols/wlr-screencopy-unstable-v1.xml', 'wlr-screencopy-unstable-v1', '', false],
  # Both are absent from wayland-protocols 1.49: input-method only exists as v1 and
  # virtual-keyboard does not exist, so they are vendored from wlroots.
  ['protocols/input-method-unstable-v2.xml', 'input-method-unstable-v2', '', false],
  ['protocols/virtual-keyboard-unstable-v1.xml', 'virtual-keyboard-unstable-v1', '', false],
  ['stable/viewporter/viewporter.xml', 'viewporter', '', true],
  ['stable/presentation-time/presentation-time.xml', 'presentation-time', '', true],
  ['staging/fractional-scale/fractional-scale-v1.xml', 'fractional-scale-v1', '', true],
  ['staging/cursor-shape/cursor-shape-v1.xml', 'cursor-shape-v1', '', true],
  ['staging/single-pixel-buffer/single-pixel-buffer-v1.xml', 'single-pixel-buffer-v1', '', true],
  ['staging/content-type/content-type-v1.xml', 'content-type-v1', '', true],
  ['staging/alpha-modifier/alpha-modifier-v1.xml', 'alpha-modifier-v1', '', true],
  ['staging/fifo/fifo-v1.xml', 'fifo-v1', '', true],
  ['unstable/pointer-gestures/pointer-gestures-unstable-v1.xml', 'pointer-gestures-unstable-v1', '', true],
  ['staging/pointer-warp/pointer-warp-v1.xml', 'pointer-warp-v1', '', true],
  ['staging/linux-drm-syncobj/linux-drm-syncobj-v1.xml', 'linux-drm-syncobj-v1', '', true],
  ['staging/tearing-control/tearing-control-v1.xml', 'tearing-control-v1', '', true],
  ['unstable/relative-pointer/relative-pointer-unstable-v1.xml', 'relative-pointer-unstable-v1', '', true],
  ['unstable/pointer-constraints/pointer-constraints-unstable-v1.xml', 'pointer-constraints-unstable-v1', '', true],
  ['unstable/idle-inhibit/idle-inhibit-unstable-v1.xml', 'idle-inhibit-unstable-v1', '', true],
  ['unstable/keyboard-shortcuts-inhibit/keyboard-shortcuts-inhibit-unstable-v1.xml', 'keyboard-shortcuts-inhibit-unstable-v1', '', true],
  # Vendored rather than taken from wayland-protocols: the interface gained three requests
  # in version 2, and a distribution copy older than that generates a struct without them.
  ['protocols/text-input-unstable-v3.xml', 'text-input-unstable-v3', '', false],
  ['staging/xdg-activation/xdg-activation-v1.xml', 'xdg-activation-v1', '', true],
  ['unstable/xdg-output/xdg-output-unstable-v1.xml', 'xdg-output-unstable-v1', '', true],
  ['unstable/xdg-decoration/xdg-decoration-unstable-v1.xml', 'xdg-decoration-unstable-v1', '', true],
  ['unstable/primary-selection/primary-selection-unstable-v1.xml', 'primary-selection-unstable-v1', '', true],
  ['staging/xwayland-shell/xwayland-shell-v1.xml', 'xwayland-shell-v1', '', true],
  ['unstable/tablet/tablet-unstable-v2.xml', 'tablet-unstable-v2', '', true],
  ['staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml', 'ext-foreign-toplevel-list-v1', '', true],
  ['staging/ext-idle-notify/ext-idle-notify-v1.xml', 'ext-idle-notify-v1', '', true],
  ['staging/xdg-toplevel-icon/xdg-toplevel-icon-v1.xml', 'xdg-toplevel-icon-v1', '', true],
  ['protocols/wlr-output-power-management-unstable-v1.xml', 'wlr-output-power-management-unstable-v1', 'ns', false],
  ['protocols/wlr-gamma-control-unstable-v1.xml', 'wlr-gamma-control-unstable-v1', 'ns', false],
  # Frame timing, sandbox identity, virtual pointer, clipboard managers, toplevel lists
  ['staging/commit-timing/commit-timing-v1.xml', 'commit-timing-v1', '', true],
  ['staging/security-context/security-context-v1.xml', 'security-context-v1', '', true],
  ['staging/ext-data-control/ext-data-control-v1.xml', 'ext-data-control-v1', '', true],
  ['protocols/wlr-virtual-pointer-unstable-v1.xml', 'wlr-virtual-pointer-unstable-v1', 'ns', false],
  ['protocols/wlr-data-control-unstable-v1.xml', 'wlr-data-control-unstable-v1', 'ns', false],
  ['protocols/wlr-foreign-toplevel-management-unstable-v1.xml', 'wlr-foreign-toplevel-management-unstable-v1', 'ns', false],
  # Workspaces, session lock and capture
  ['staging/ext-workspace/ext-workspace-v1.xml', 'ext-workspace-v1', '', true],
  ['staging/ext-session-lock/ext-session-lock-v1.xml', 'ext-session-lock-v1', '', true],
  ['staging/ext-image-capture-source/ext-image-capture-source-v1.xml', 'ext-image-capture-source-v1', '', true],
  ['staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml', 'ext-image-copy-capture-v1', '', true],
]

gen_script = join_paths(meson.current_source_dir(), 'build-aux/gen-proto.sh')
proto_c = []
foreach p : proto_entries
  xml = p[3] ? join_paths(wp_dir, p[0]) : join_paths(meson.current_source_dir(), p[0])
  gens = custom_target(p[1],
    input: xml,
    output: [p[1] + '-server-protocol.h', p[1] + '-protocol.c'],
    command: [gen, gen_script, '@INPUT@', '@OUTDIR@', p[1], p[2]])
  proto_c += gens[1]
endforeach

# Single translation unit: src/main.cpp is a list of includes in the original order,
# plus main(). The modules share static globals, so do not split them into separate
# .cpp files before turning those globals into extern declarations.
executable('sfwm',
  'src/main.cpp',
  proto_c,
  include_directories: inc,
  link_args: ld_extra,
  install: false)
