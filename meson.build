# sfwm —— 把 SurfaceFlinger 当合成器用的 Wayland compositor
#
# 构建策略上有一个必须照抄旧 build.sh 的点：
#   只用 pkg-config 的**库**，不用它的 cflags。
#   Termux 各 .pc 里带 -I$PREFIX/include，而这个目录下还有一份 sys/cdefs.h，
#   一旦排在 bionic 前面，就会出现 memchr/strchr 重定义、attributes 不允许等怪错。
#   所以这里全部用 find_library + 手动 -idirafter。
project('sfwm', ['c', 'cpp'],
  default_options: ['cpp_std=gnu++17', 'buildtype=release', 'warning_level=0'])

prefix  = get_option('prefix')
cc      = meson.get_compiler('cpp')
gen     = find_program('sh')

# ---- 头文件路径（-idirafter 保证排在 bionic 之后）-------------------------
add_project_arguments(
  '-idirafter', join_paths(prefix, 'include'),
  '-idirafter', join_paths(prefix, 'include/libdrm'),
  language: ['c', 'cpp'])

# ---- 库（完全照抄旧 build.sh：只用 -L + -l，不碰 pkg-config）-------------
libdir  = join_paths(prefix, 'lib')
gccdir  = join_paths(prefix, 'opt/gcc-17/lib/gcc/aarch64-linux-android/17.0.0')
ld_extra = [
  '-L' + libdir,
  '-L' + gccdir,
  '-lwayland-server', '-ldrm', '-lxkbcommon', '-landroid', '-llog',
  '-static-libgcc',
  '-Wl,-rpath,' + libdir,
]

# ---- 协议生成 ------------------------------------------------------------
proto = []
foreach p : [
    ['protocols/wayland-drm.xml',                 'wayland-drm',                 ''],
    ['protocols/wlr-layer-shell-unstable-v1.xml', 'wlr-layer-shell-unstable-v1', 'ns'],
    ['protocols/wlr-screencopy-unstable-v1.xml',  'wlr-screencopy-unstable-v1',  ''],
  ]
  gens = custom_target(p[1],
    input: p[0],
    output: [p[1] + '-server-protocol.h', p[1] + '-protocol.c'],
    command: [gen, join_paths(meson.current_source_dir(), 'build-aux/gen-proto.sh'),
              '@INPUT@', '@OUTDIR@', p[1], p[2]])
  proto += static_library(p[1], gens)
endforeach

# ---- 主程序 --------------------------------------------------------------
executable('sfwm',
  'src/sfwm.cpp',
  proto,
  link_args: ld_extra,
  install: false)
