# sfwm

用 **SurfaceFlinger** 当合成器的 Wayland compositor：每个 `xdg_toplevel` / `wl_surface`
对应一个 `sf::Layer`，像素由 CPU 从客户端 dmabuf/shm 搬进 `ANativeWindow`。

> 起因：本机内核 `CONFIG_VT` 未开、GPU 只有 KGSL（无 DRM），所以拿不到 DRM master，
> 传统合成器（wlroots/Hyprland）无法直接跑。SurfaceFlinger 是唯一能用的显示通路。

## 现状

`src/sfwm.cpp` 是一个 **5631 行的单文件**（MVP 产物，能跑但结构糟糕）。
本仓库正在进行 **只搬不重写** 的拆分：

| 目标模块 | 现在在哪 |
|---|---|
| `backend/sf_layer`   SurfaceFlinger 图层事务（手写 dlsym + asm 桥） | `src/sf_layer.hpp` ✅ 已独立 |
| `protocol/screencopy` zwlr_screencopy_manager_v1 | `src/sc_screencopy.hpp` ✅ 已独立 |
| `render/blit`        像素搬运（dmabuf/shm、格式、alpha、看门狗） | `src/sfwm.cpp` ⏳ |
| `input/gestures`     点击/长按/捏合/缩放 | `src/sfwm.cpp` ⏳ |
| `compositor/*`       server / surface / output | `src/sfwm.cpp` ⏳ |
| `shell/*`            xdg_shell / layer_shell / popup | `src/sfwm.cpp` ⏳ |
| `protocol/*`         data_device / text_input / presentation … | `src/sfwm.cpp` ⏳ |

## 构建

```sh
meson setup build && ninja -C build          # 需要 meson + ninja
# 或者沿用旧的单脚本构建：
sh build-aux/build.sh
```

## 几个踩过的硬坑（别删注释里的说明）

1. **`extern "C" {` 包住了协议头** → 任何被它包住的头文件里不能出现 C++ 特性
   （`namespace`/模板会报 `templates must have C++ linkage`）。
2. **全局注册那段代码在 `main()` 内部** → 不要往 `wl_global_create` 旁边插实现。
3. **协议生成要 sed**：wlr-layer-shell 的参数名 `namespace` 是 C++ 关键字。
4. **链接要 `-static-libgcc` + `-L.../opt/gcc-17/...`**：Termux 的 clang 默认找 libgcc。

## 架构上的三个关键决定

- **缩放走合成器侧变换**：捏合只改图层 `dst` 几何（SF 用 GPU 采样），不向客户端发
  configure；抬手才发一次，避免 swapchain 重建导致的闪烁。
- **读客户端缓冲区延后一帧**：客户端用 GPU 渲染，立刻读会拿到画一半的缓冲区
  （表现为播放视频时闪）。`View::read_next` + 看门狗。
- **图层格式用 `HAL_PIXEL_FORMAT_BGRA_8888`**：和客户端 ARGB8888 内存布局一致，
  省掉逐像素 R/B 交换；`eLayerOpaque` 在 Android 10+ 已废弃，opaque 只能靠格式。
