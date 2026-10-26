// prelude.hpp
//
// 标准头 + 协议头 + wl_shm 的 fd 帮手
//
// 从 sfwm.cpp 原样搬出（只搬不改，逻辑零差异）。
// 依赖包含顺序：本文件用到的东西必须由**前面的**模块提供，
// 因此它在 sfwm.cpp 里的 include 位置就是它原来在文件里的位置。
#pragma once

// sfwm.cpp —— 把 SurfaceFlinger 当窗口管理器的 Wayland compositor
//
// 设计：不自已合成像素。每个 xdg_toplevel 对应一个独立的 SurfaceFlinger 图层，
//       position / z-order / 显隐 全部通过 SurfaceComposerClient::Transaction 下发给 SF，
//       由 SF 的硬件合成器完成混叠。我们只负责 wl_shm 像素搬运 + 窗口策略。
//
// 构建：见 build.sh（需要 wayland-scanner 生成 xdg-shell 协议代码）

#include "sf_layer.hpp"

// wayland 的协议头没有 extern "C" 保护，而生成的 .c 是按 C 编的，
// 这里必须显式包一层，否则 xdg_wm_base_interface 之类会变成 C++ 修饰名 → 链接失败。
extern "C" {
#include <wayland-server-core.h>
#include "sc_screencopy.hpp"
#include <wayland-server-protocol.h>
#include "xdg-shell-server-protocol.h"
#include "linux-dmabuf-v1-server-protocol.h"
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <cerrno>
#include <cstring>
#include "wayland-drm-server-protocol.h"
#include "viewporter-server-protocol.h"
#include "presentation-time-server-protocol.h"
#include "fractional-scale-v1-server-protocol.h"
#include "cursor-shape-v1-server-protocol.h"
#include "single-pixel-buffer-v1-server-protocol.h"
#include "tearing-control-v1-server-protocol.h"
#include "relative-pointer-unstable-v1-server-protocol.h"
#include "pointer-constraints-unstable-v1-server-protocol.h"
#include "idle-inhibit-unstable-v1-server-protocol.h"
#include "keyboard-shortcuts-inhibit-unstable-v1-server-protocol.h"
#include "text-input-unstable-v3-server-protocol.h"
#include "xdg-output-unstable-v1-server-protocol.h"
#include "xdg-decoration-unstable-v1-server-protocol.h"
#include "primary-selection-unstable-v1-server-protocol.h"
#include "xwayland-shell-v1-server-protocol.h"
#include "wlr-layer-shell-unstable-v1-server-protocol.h"
#include "xdg-activation-v1-server-protocol.h"
#include "content-type-v1-server-protocol.h"
#include "alpha-modifier-v1-server-protocol.h"
#include "fifo-v1-server-protocol.h"
#include "pointer-gestures-unstable-v1-server-protocol.h"
#include "pointer-warp-v1-server-protocol.h"
#include "linux-drm-syncobj-v1-server-protocol.h"
#include <xf86drm.h>
}

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <stddef.h>
#include <time.h>
#include <unistd.h>

#include <xkbcommon/xkbcommon.h>

#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// 给客户端传 fd 时**不能**用 memfd_create。
//   memfd 是匿名 shmem，安全标签继承「创建者的域」—— sfwm 是 root（u:r:ksu:s0，
//   无 MLS 类别）；而 Termux 应用进程是 u:r:runas_app:s0:c512,c768。
//   跨 MLS 类别接收 fd 会被 security_file_receive 拒绝，Android 上这条规则是
//   dontaudit，所以 dmesg 里看不到任何 avc —— 表现只是客户端少了个 fd：
//       XWAYLAND: file descriptor expected, object (22), message keymap(uhu)
//       (EE) failed to read Wayland events: Invalid argument   → Xwayland 直接退出
//       eglInitialize failed                                   → OBS 视频初始化失败
//   ⇒ 改成在 XDG_RUNTIME_DIR（Termux 数据目录）里建普通文件：标签是
//     u:object_r:app_data_file:s0:c512,c768，与 runas_app 类别一致 → 传输放行。
//   这样 Xwayland / OBS / java 全都能留在 runas_app 域，不需要塞进 ksu。
// ─────────────────────────────────────────────────────────────────────────────
static int sfwm_rt_file(const char *name, size_t size) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || !*rt) return -1;
    static int seq = 0;
    size_t n = strlen(rt) + strlen(name) + 32;
    char *p = (char *)malloc(n);
    if (!p) return -1;
    snprintf(p, n, "%s/%s-%d-%d", rt, name, (int)getpid(), ++seq);
    int fd = open(p, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        fprintf(stderr, "[sf] 建 %s 失败: %s\n", p, strerror(errno));
        free(p);
        return -1;
    }
    if (size > 0 && ftruncate(fd, (off_t)size) < 0) {
        fprintf(stderr, "[sf] ftruncate %s 失败: %s\n", p, strerror(errno));
        close(fd);
        free(p);
        return -1;
    }
    chmod(p, 0644);
    // 立刻 unlink：fd 仍然有效，内容随最后一个引用消失，不占目录
    unlink(p);
    free(p);
    return fd;
}

