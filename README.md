# sfwm

[![ci](https://github.com/woshixyysm/SurfaceFlinger-Window-Manager/actions/workflows/ci.yml/badge.svg)](https://github.com/woshixyysm/SurfaceFlinger-Window-Manager/actions/workflows/ci.yml)

用 **SurfaceFlinger** 当合成器的 Wayland compositor：每个 `xdg_toplevel` / `wl_surface`
对应一个 `sf::Layer`，像素由 CPU 从客户端 dmabuf/shm 搬进 `ANativeWindow`。

> 起因：本机内核 `CONFIG_VT` 未开、GPU 只有 KGSL（无 DRM），所以拿不到 DRM master，
> 传统合成器（wlroots/Hyprland）无法直接跑。SurfaceFlinger 是唯一能用的显示通路。

## 结构

拆分原则：**只搬不重写**。切点全是「顺序边界」，每个模块的 include 位置
就是它原来在单文件里的位置，因此预处理后的 TU 与原来等价。
能用 `tools/verify_split.py` 那种办法把模块拼回去逐行比对来证明无损（已逐行一致）。

```
src/
  sfwm.cpp                 入口：include 表（顺序 = 原文件顺序）+ main
  view.hpp                 struct View（一个 wl_surface 的全部状态 + 几何/堆叠助手）
  sf_layer.hpp             SurfaceFlinger 图层事务（手写 dlsym + asm 桥）
  sc_screencopy.hpp        zwlr_screencopy_manager_v1
  core/prelude.hpp         标准头 + 协议头 + wl_shm fd 帮手
  core/config.hpp          配置全局 + 缓冲/连接/Server 结构体
  core/trace.hpp           结构化跟踪（SFWM_TRACE）
  render/cursor.hpp        光标几何绘制 + Xcursor 主题解析 + 上屏
  render/testpat.hpp       自检图案（SFWM_TESTPAT）
  compositor/surface.hpp   wl_region / wl_surface / wl_shm
  protocol/linux_dmabuf.hpp, wl_drm.hpp, compositor.hpp, subcompositor.hpp,
           output.hpp, misc_ext.hpp, misc_ext2.hpp, syncobj.hpp,
           presentation.hpp, activation.hpp, xwayland.hpp
  input/seat.hpp            wl_seat / pointer / touch 绑定
  input/keyboard.hpp       wl_keyboard + text_input_v3 + IME + 控制命令
  input/inject.hpp         按键注入 socket（sfwmc 走这里）
  input/pointer.hpp        指针 / 鼠标按钮 / 命中测试 / 抬升
  input/touch.hpp          触摸状态机 + 手势 + handle_frame + /dev/input
  shell/layer_shell.hpp    zwlr_layer_shell_v1（面板/托盘）
  shell/xdg_shell.hpp      xdg_wm_base / toplevel / popup / configure
```

> ⚠️ 这些模块目前仍是 **单 TU**（`sfwm.cpp` 只做 include），因为它们共享 `static` 全局。
> 下一步拆成独立 `.cpp` 时，必须先把全局改成 `extern` 声明 + 一处定义，
> 那是有真实风险的改动，和本轮“零行为差异”不是同一件事。

## 构建

```sh
# Termux / Android 上必须显式指定 clang：默认的 c++ 是 GCC 17（libstdc++），
# 会把源码里乱序的 designated initializer 当错误。
CC=clang CXX=clang++ meson setup build --prefix=$PREFIX
ninja -C build                               # 产物 build/sfwm
```

`--prefix` 必须给（默认 `/usr/local` 会让 `-idirafter` 全部落空）。
协议清单（28 个 = 3 个私有 + 25 个系统 wayland-protocols）已全部写进 `meson.build`。

## CI 能做什么、不能做什么

这个项目的编译**只能在 Termux / Android 上做**（要 bionic、`libandroid`、`liblog`，
以及 SurfaceFlinger 的 Binder ABI），Linux runner 编不了。所以 `.github/workflows/ci.yml`
只跑不需要 Android 环境也能证明的东西：

| 检查 | 内容 |
|---|---|
| include 解析 | 本地 include 必须存在；协议头可以是待生成的，但必须在 `meson.build` 清单里 |
| 无孤儿 / 无环 | `src/` 下每个 `.hpp` 从 `sfwm.cpp` 都可达；模块之间不能 include 成环 |
| `#pragma once` | 每个模块头都要有 |
| 无生成物入库 | `*-protocol.c`、`*-server-protocol.h`、`*.o`、`sfwm` 一律不许提交 |
| 协议生成 | 用 `wayland-scanner` 把 28 个 XML 逐个生成一遍（装了 `libwayland-bin` + `wayland-protocols`） |
| **交叉编译** | 用 [android-ndk-custom](https://github.com/HomuHomu833/android-ndk-custom) r30 + `deps/` 里那份 Termux 头库，把 `sfwm` 真编成 aarch64-android 二进制 |

交叉编译为什么能成：代码对 Android 的依赖很少 —— 只有 `android/native_window.h`（NDK sysroot 里有），
SurfaceFlinger 那套是**手写 dlsym ABI**，不需要链接 `libgui`。所以只差三个库的头和 `.so`，
而 `deps/termux-deps-aarch64.tar.xz`（432 KB）就是**设备上在跑的那一份**（wayland 1.25.0 /
xkbcommon 1.13.2 / libdrm 2.4.134），ABI 天然一致，不依赖任何外部软件源。详见 `deps/README.md`。

> **AOSP 不用拉。** NDK 本身就是 AOSP 的工具链产物，bionic 和 `native_window.h` 里都有；
> 我们不 include 任何 `gui/`、`binder/` 头，也没链 `libgui`。拉整个 AOSP（几十 GB）
> 对编译这个 5600 行合成器没有增量价值。

> 该 job 首轮带 `continue-on-error: true`：本机无法复现 runner 上的 NDK 下载/解包路径，
> 先让失败不卡仓库。第一次绿了就把那行删掉，它就变成真闸门。

本地想跑这条交叉编译（需要 NDK）：

```sh
mkdir -p /tmp/deps && tar -xJf deps/termux-deps-aarch64.tar.xz -C /tmp/deps
NDK=/path/to/ndk DEPS=/tmp/deps sh build-aux/ci-cross-build.sh
# 产物 ci-build/sfwm
```

本地跑：

```sh
python3 tools/check_structure.py                      # 结构检查
python3 tools/check_structure.py --protogen --show-order  # 加上协议生成，并打印展开顺序
python3 tools/verify_split.py                         # 拆分无损证明（手动 workflow 里跑）
```

`tools/verify_split.py` 会拿 `389c0f6`（拆分前那次提交）的单文件跟当前模块拼回去的结果逐行比对。
它只在手动触发（workflow_dispatch）时跑 —— 模块一旦有意修改（修 bug / 加功能），这项就该失败，
它是一次可复现的证据，不是长期约束。

## 几个踩过的硬坑（别删注释里的说明）

1. **`extern "C" {` 包住了协议头** → 任何被它包住的头文件里不能出现 C++ 特性
   （`namespace`/模板会报 `templates must have C++ linkage`）。
2. **全局注册那段代码在 `main()` 内部** → 不要往 `wl_global_create` 旁边插实现。
3. **协议生成要 sed**：wlr-layer-shell 的参数名 `namespace` 是 C++ 关键字。
4. **链接要 `-static-libgcc` + `-L.../opt/gcc-17/...`**：Termux 的 clang 默认找 libgcc。
5. **编译器必须是 clang++**：meson 默认 `c++` = GCC 17，报
   `designator order for field 'wp_fifo_v1_interface::set_barrier' does not match
   declaration order`（源码里 designated initializer 是乱序的，clang 在 `-std=c++17`
   下接受，GCC 不接受）。同时 `-D_GLIBCXX_ASSERTIONS=1` 是 libstdc++ 的信号，
   看到它说明选错了编译器。
6. **生成的协议 `.c` 不能放进 `static_library`**：`wl_interface` 结构体在 C++ 目标里
   会变成内部链接 → 链接期一堆 `undefined symbol: xxx_interface`。
   必须作为可执行文件的源直接编译（meson 按扩展名自动用 C 编）。
7. **不要用 `pkg-config --cflags`**：`-I$PREFIX/include` 会把另一份 `sys/cdefs.h`
   顶到 bionic 前面。只用它的 `--libs`，头文件一律 `-idirafter`。

## 架构上的三个关键决定

- **缩放走合成器侧变换**：捏合只改图层 `dst` 几何（SF 用 GPU 采样），不向客户端发
  configure；抬手才发一次，避免 swapchain 重建导致的闪烁。
- **读客户端缓冲区延后一帧**：客户端用 GPU 渲染，立刻读会拿到画一半的缓冲区
  （表现为播放视频时闪）。`View::read_next` + 看门狗。
- **图层格式用 `HAL_PIXEL_FORMAT_BGRA_8888`**：和客户端 ARGB8888 内存布局一致，
  省掉逐像素 R/B 交换；`eLayerOpaque` 在 Android 10+ 已废弃，opaque 只能靠格式。
