# sfwm —— 把 SurfaceFlinger 当合成器用的 Wayland compositor
#
# 构建上必须照抄旧 build.sh 的四条约定（都是踩出来的）：
#   1. 用 clang++，**不能**用默认的 c++（那是 GCC 17 / libstdc++）；
#   2. 只用 pkg-config 的库，不用它的 cflags —— 否则 -I$PREFIX/include
#      会把另一份 sys/cdefs.h 顶到 bionic 前面；
#   3. 协议生成的 .c 必须按 C 直接编进可执行文件（C++ 里 wl_interface 会变内部链接）；
#   4. cpp_std=c++17（严格），源码里有乱序 designated initializer。
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
gccdir  = join_paths(prefix, 'opt/gcc-17/lib/gcc/aarch64-linux-android/17.0.0')
ld_extra = [
  '-L' + libdir, '-L' + gccdir,
  '-lwayland-server', '-ldrm', '-lxkbcommon', '-landroid', '-llog',
  '-static-libgcc', '-Wl,-rpath,' + libdir,
]

# xdg-shell 等系统协议在 share/wayland-protocols 下；三个私有协议在 protocols/
proto_entries = [
  ['stable/xdg-shell/xdg-shell.xml', 'xdg-shell', '', true],
  ['stable/linux-dmabuf/linux-dmabuf-v1.xml', 'linux-dmabuf-v1', '', true],
  ['protocols/wayland-drm.xml', 'wayland-drm', '', false],
  ['protocols/wlr-layer-shell-unstable-v1.xml', 'wlr-layer-shell-unstable-v1', 'ns', false],
  ['protocols/wlr-screencopy-unstable-v1.xml', 'wlr-screencopy-unstable-v1', '', false],
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
  ['unstable/text-input/text-input-unstable-v3.xml', 'text-input-unstable-v3', '', true],
  ['staging/xdg-activation/xdg-activation-v1.xml', 'xdg-activation-v1', '', true],
  ['unstable/xdg-output/xdg-output-unstable-v1.xml', 'xdg-output-unstable-v1', '', true],
  ['unstable/xdg-decoration/xdg-decoration-unstable-v1.xml', 'xdg-decoration-unstable-v1', '', true],
  ['unstable/primary-selection/primary-selection-unstable-v1.xml', 'primary-selection-unstable-v1', '', true],
  ['staging/xwayland-shell/xwayland-shell-v1.xml', 'xwayland-shell-v1', '', true],
  ['unstable/tablet/tablet-unstable-v2.xml', 'tablet-unstable-v2', '', true],
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

executable('sfwm',
  'src/sfwm.cpp',
  proto_c,
  link_args: ld_extra,
  install: false)
