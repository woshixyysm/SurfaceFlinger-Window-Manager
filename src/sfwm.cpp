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

// ---------------------------------------------------------------- 配置
static int32_t g_out_w = 1272, g_out_h = 2800;
static int32_t g_base_z = 1000;
// 点击穿透：true = 我们的图层不再接收触摸，全部漏给下面的安卓界面
static bool g_clickthrough = false;
// 拖拽中：用来统计客户端是不是还在重绘（重绘 + 位置事务不同步 = 闪烁）
static bool g_in_drag = false;
// 吸附（拖拽过程中对齐屏幕边/中线），可在运行时用注入器 snap on|off / snap <dp> 调
static bool g_snap_on = true;
static int32_t g_snap_band_dp = 24;
static int g_snap_bits = 0;      // 1=贴左 2=居中 4=贴右 8=贴顶
static const char *g_socket = "sfwm-0";
static bool g_grab = false;
static int32_t g_scale = 3;   // 560dpi 屏桌面程序默认太小，按 3x 渲染

// ---- 安卓软键盘（走 LSPosed 模块里的悬浮窗宿主 SfwmImeService）
static int32_t g_ime_h = 0;    // 键盘高度（屏幕像素），0 = 没键盘
static bool    g_ime_on = false;
// IME 客户端保持的那条长连接。方向反过来之后就不需要 settings / am / 文件轮询了：
// sfwm 直接往这条连接上写 activate / deactivate，跟 Wayland 服务端下发事件一个意思。
static int     g_ime_fd = -1;
// 底图带的内部序号：整屏窗口按映射先后排序，永远排在内容窗口之下
static int32_t g_bg_seq = 0;

static void ime_send(bool show);          // 前向声明：ti_enable/ti_disable 要用
static void ti_force_disable();           // 同上

// ---- wl_shm 自定义缓冲（wayland 自带的不支持 offset）
static const uint32_t SHMBUF_MAGIC = 0x53464246u;  // 'SBFB'

// wl_shm 池：客户端可以先 create_pool -> create_buffer -> destroy(pool)，
// buffer 依然有效。所以池必须引用计数，最后一个使用者释放时才 munmap。
struct ShmPool {
    int fd = -1;
    int32_t size = 0;
    uint8_t *base = nullptr;
    int refs = 1;          // 池资源自己 1 份 + 每个还活着的 buffer 一份
};

static void shm_pool_unref(ShmPool *p) {
    if (!p) return;
    if (--p->refs > 0) return;
    if (p->base) munmap(p->base, p->size);
    if (p->fd >= 0) close(p->fd);
    delete p;
}

struct ShmBuf {
    uint32_t magic = SHMBUF_MAGIC;
    ShmPool *pool = nullptr;   // 引用一份池，保证 base 不被提前 munmap
    int32_t offset = 0;
    int32_t w = 0, h = 0, stride = 0;
    uint32_t format = 0;
};

// ---- dmabuf 缓冲（zwp_linux_dmabuf_v1）
static const uint32_t DMABUF_MAGIC = 0x44464246u;  // 'DFBF'

struct DmabufBuf {
    uint32_t magic = DMABUF_MAGIC;
    int32_t w = 0, h = 0;
    uint32_t drm_format = 0;
    uint64_t modifier = 0;
    int nplanes = 0;
    int fds[4] = {-1, -1, -1, -1};
    uint32_t stride[4] = {0, 0, 0, 0};
    uint32_t offset[4] = {0, 0, 0, 0};
};

struct View;

// 注入命令的一条连接
struct KeyConn {
    int fd = -1;
    wl_event_source *src = nullptr;
    std::string buf;
};

struct Server {
    wl_display *dpy = nullptr;
    wl_event_loop *loop = nullptr;
    std::vector<View *> views;
    int32_t next_z = 0;
    // 输入
    std::vector<wl_resource *> pointers;
    std::vector<wl_resource *> touches;
    std::vector<wl_resource *> keyboards;
    std::vector<wl_resource *> text_inputs;   // zwp_text_input_v3
    View *pointer_focus = nullptr;
    View *kb_focus = nullptr;       // 键盘焦点（点一下哪个窗口就给哪个）
    View *cursor_view = nullptr;
    int32_t cursor_hot_x = 0, cursor_hot_y = 0;
    int32_t pointer_x = 0, pointer_y = 0;   // 最后的全局指针位置（屏幕坐标）
    View *move_view = nullptr;      // xdg_toplevel.move 发起的交互式移动
    // 键盘注入 socket
    int key_listen_fd = -1;
    std::string key_sock_path;
    std::vector<KeyConn *> key_conns;
};

static Server g_srv;

// ---------------------------------------------------------------- 结构化跟踪
// SFWM_TRACE=1 时把「几何变化 + 命中判定 + 手势状态」写到文件，便于离线分析。
// 每条都带 reason，能直接看出是 map / fit / move / resize / layer / ime 哪一步改的。
static FILE   *g_trace_fp = nullptr;
static bool    g_trace_on = false;
static const char *g_reason = "?";
// 手势进行中（MOVE/RESIZE/PINCH）：期间几何更新要限流，否则每帧改裁剪框 = 闪
static bool g_in_gesture = false;
static int  touch_owner_value(void);   // 定义在触摸状态机那一段（g_owner 声明之后）

static int64_t trace_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void trace_init() {
    const char *e = getenv("SFWM_TRACE");
    if (!e || !*e || *e == '0') return;
    g_trace_fp = fopen("/data/local/tmp/sfwm-trace.log", "w");
    g_trace_on = g_trace_fp != nullptr;
    if (g_trace_on) printf("[trace] 输出到 /data/local/tmp/sfwm-trace.log\n");
}

static void trace(const char *fmt, ...) {
    if (!g_trace_on) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(g_trace_fp, "%8lld ", (long long)trace_ms());
    vfprintf(g_trace_fp, fmt, ap);
    fputc('\n', g_trace_fp);
    va_end(ap);
    fflush(g_trace_fp);
}

// 是否有客户端自带光标（Hyprland 的 wl_pointer.set_cursor）。
// 有的话就不能再叠 sfwm 自己的内置光标，否则屏幕上是两个箭头。
static bool g_have_client_cursor = false;

// 按下时锁定的视图。
// 为什么需要：抬起时如果只查 g_srv.pointer_focus，而手指在按住期间稍微滑出窗口
// （或触点多指切换会把主指换掉），focus 会被清成 null -> UP 里 `if (!v) return;`
// 直接把 release 吞了。客户端（Qt/GTK）于是永远以为左键是按下的，
// 之后每一攣点击都被当成“拖动中的 motion” —— 表现就是“点什么都没反应”。
static View *g_btn_view = nullptr;

// 角带拖拽的“延迟确认”：
// 以前只要按在窗口四个角的带子里（32*scale = 96px）就立即 OWNER_RESIZE，
// 导致窗口底部的按钮（正好压在底边那条带里）永远点不动 —— 按下去变成改尺寸。
// 现在先**布防**，手指真的移动超阈值才转 resize；没动就照常把点击发给客户端。
static int32_t g_arm_corner = 0;
static int32_t g_arm_x = 0, g_arm_y = 0;

// 先声明，View::map()/blit() 里要用到（实现在后面）
static void kb_set_focus(View *v);
static void toplevel_send_configure(View *v);
static int32_t client_scale(View *v);
static void layer_apply_anchor(View *v);
static void pres_send_presented(View *v);
static void sync_wait_acquire(View *v);
static void sync_signal_release(View *v);

// ─────────────────────────────────────────────────────────────────────────────
// 内置光标
//
// 为什么必须自己画：
//   * `wl_pointer.set_cursor` 只有「自己管光标」的工具箱才发（GTK/Qt 会），
//     而 Xwayland 的根窗口、AWT/Swing 窗口基本从不发；
//   * `wp_cursor_shape_v1.set_shape` 只给一个**形状编号**，不给像素 —— 合成器
//     必须自己按编号去找图。以前 cs_dev_set_shape() 是个空壳，所以桌面区域
//     根本看不到光标。
//
// 做法：24x24 逻辑像素程序化光栅化（白填充 + 黑描边），贴到一块独立 SF 图层上，
// 指针一动就重绘。物理尺寸按 g_scale 放大，热点也一起乘。
// ─────────────────────────────────────────────────────────────────────────────
static const int32_t CUR_N = 24;                 // 兜底箭头的逻辑尺寸
static sf::Layer     g_cur_layer;
static uint32_t     *g_cur_px = nullptr;         // ARGB8888 位图
static int32_t       g_cur_w = 0, g_cur_h = 0;
static int32_t       g_cur_hx = 0, g_cur_hy = 0; // 热点（位图坐标系）
static int32_t       g_cur_mul = 1;              // 放大倍数；0 = 按 g_scale
static int32_t       g_cur_lw = 0, g_cur_lh = 0; // 图层当前物理尺寸
static uint32_t      g_cur_shape = 1;            // wp_cursor_shape_v1，1 = default
static bool          g_cur_built = false;
static bool          g_cur_shown = false;
static int32_t       g_cur_x = 0, g_cur_y = 0;   // 最后的指针位置（物理）

#define CUR_OPQ 0xFF000000u   // 不透明黑（描边）
#define CUR_WHT 0xFFFFFFFFu   // 白（填充）
#define CUR_CLR 0x00000000u   // 全透明

static inline void cur_plot(int32_t x, int32_t y, uint32_t c) {
    if (x < 0 || y < 0 || x >= g_cur_w || y >= g_cur_h) return;
    g_cur_px[y * g_cur_w + x] = c;
}

static void cur_rect(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t c) {
    if (x0 > x1) { int32_t t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int32_t t = y0; y0 = y1; y1 = t; }
    for (int32_t y = y0; y <= y1; y++)
        for (int32_t x = x0; x <= x1; x++) cur_plot(x, y, c);
}

static void cur_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t c) {
    int32_t dx = x1 - x0, dy = y1 - y0;
    int32_t ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    int32_t sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int32_t err = ax - ay;
    for (;;) {
        cur_plot(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int32_t e2 = err * 2;
        if (e2 > -ay) { err -= ay; x0 += sx; }
        if (e2 < ax)  { err += ax; y0 += sy; }
    }
}

// 扫描线奇偶填充（pts 是 x0,y0,x1,y1,... 展平）
static void cur_poly(const int32_t *p, int n, uint32_t c) {
    if (n > 16) n = 16;
    int32_t miny = 1 << 30, maxy = -(1 << 30);
    for (int i = 0; i < n; i++) {
        int32_t y = p[2 * i + 1];
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
    }
    if (miny < 0) miny = 0;
    if (maxy > g_cur_h - 1) maxy = g_cur_h - 1;
    int32_t xs[16];
    for (int32_t y = miny; y <= maxy; y++) {
        int m = 0;
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            int32_t y0 = p[2 * i + 1], y1 = p[2 * j + 1];
            if (y0 == y1) continue;
            if ((y >= y0 && y < y1) || (y >= y1 && y < y0)) {
                double t = (double)(y - y0) / (double)(y1 - y0);
                int32_t x = (int32_t)(p[2 * i] + t * (p[2 * j] - p[2 * i]) + 0.5);
                if (m < 16) xs[m++] = x;
            }
        }
        for (int a = 0; a < m; a++)
            for (int b = a + 1; b < m; b++)
                if (xs[b] < xs[a]) { int32_t t = xs[a]; xs[a] = xs[b]; xs[b] = t; }
        for (int a = 0; a + 1 < m; a += 2)
            for (int32_t x = xs[a]; x <= xs[a + 1]; x++) cur_plot(x, y, c);
    }
}

// 黑描边 + 白填充：把多边形顶点朝质心收缩 ~1.2px 再填白
static void cur_poly_ol(const int32_t *p, int n) {
    cur_poly(p, n, CUR_OPQ);
    int32_t q[32];
    int32_t cx = 0, cy = 0;
    for (int i = 0; i < n; i++) { cx += p[2 * i]; cy += p[2 * i + 1]; }
    cx /= n; cy /= n;
    for (int i = 0; i < n; i++) {
        double dx = p[2 * i] - cx, dy = p[2 * i + 1] - cy;
        double len = sqrt(dx * dx + dy * dy);
        double k = len > 1.8 ? (len - 1.2) / len : 0.0;
        q[2 * i] = cx + (int32_t)(dx * k);
        q[2 * i + 1] = cy + (int32_t)(dy * k);
    }
    cur_poly(q, n, CUR_WHT);
}

static void cur_circle(int32_t cx, int32_t cy, int32_t r, int32_t thick, uint32_t c) {
    for (int32_t y = cy - r; y <= cy + r; y++) {
        for (int32_t x = cx - r; x <= cx + r; x++) {
            int32_t dx = x - cx, dy = y - cy;
            int32_t d2 = dx * dx + dy * dy;
            if (d2 <= r * r && d2 >= (r - thick) * (r - thick)) cur_plot(x, y, c);
        }
    }
}

static void cur_disc(int32_t cx, int32_t cy, int32_t r, uint32_t c) {
    for (int32_t y = cy - r; y <= cy + r; y++) {
        for (int32_t x = cx - r; x <= cx + r; x++) {
            int32_t dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy <= r * r) cur_plot(x, y, c);
        }
    }
}

// ---- 主题加载：直接解析 Xcursor 文件
// 不用 libwayland-cursor：本机那版 wl_cursor_image 没有 data 成员，而且它需要
// 一个客户端侧的 wl_shm，我们只有服务端，读不到像素。Xcursor 格式很简单，自己读。
static const char *cursor_shape_name(uint32_t s);

// cursor-shape-v1 枚举 -> xcursor 文件名
static const char *cursor_shape_name(uint32_t s) {
    switch (s) {
    case 2:  return "context-menu";
    case 3:  return "help";
    case 4:  return "pointer";
    case 5:  return "progress";
    case 6:  return "wait";
    case 7:  return "cell";
    case 8:  return "crosshair";
    case 9:  return "text";
    case 10: return "vertical-text";
    case 11: return "alias";
    case 12: return "copy";
    case 13: return "move";
    case 14: return "no-drop";
    case 15: return "not-allowed";
    case 16: return "grab";
    case 17: return "grabbing";
    case 18: return "e-resize";
    case 19: return "n-resize";
    case 20: return "ne-resize";
    case 21: return "nw-resize";
    case 22: return "s-resize";
    case 23: return "se-resize";
    case 24: return "sw-resize";
    case 25: return "w-resize";
    case 26: return "ew-resize";
    case 27: return "ns-resize";
    case 28: return "nesw-resize";
    case 29: return "nwse-resize";
    case 30: return "col-resize";
    case 31: return "row-resize";
    case 32: return "all-scroll";
    case 33: return "zoom-in";
    case 34: return "zoom-out";
    default: return "left_ptr";
    }
}

// xcursor 的"哈希文件名"算法（主题目录里常见纯十六进制文件名）
static uint32_t xc_hash(const char *name) {
    uint32_t hash = 0, i = 0, len = (uint32_t)strlen(name);
    while (len--) {
        hash = (hash << 4) + (uint8_t)name[i];
        i++;
        uint32_t g = hash & 0xf0000000u;
        if (g) { hash ^= g >> 24; hash &= ~g; }
    }
    return hash;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// 读一个 Xcursor 文件，挑最接近 want 的尺寸
static bool xc_load_file(const char *path, int32_t want, int32_t *ow, int32_t *oh,
                         int32_t *ohx, int32_t *ohy, uint32_t **opx) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t hdr[16];
    if (fread(hdr, 1, 16, f) != 16 || rd32(hdr) != 0x72756358u) { fclose(f); return false; }
    uint32_t ntoc = rd32(hdr + 12);
    if (ntoc == 0 || ntoc > 4096) { fclose(f); return false; }

    uint32_t *bestpx = nullptr;
    int32_t best = 1 << 30, bw = 0, bh = 0, bhx = 0, bhy = 0;
    for (uint32_t i = 0; i < ntoc; i++) {
        uint8_t e[12];
        if (fread(e, 1, 12, f) != 12) break;
        uint32_t type = rd32(e), sub = rd32(e + 4), pos = rd32(e + 8);
        if (type != 0xfffd0002u) continue;              // 只要 IMAGE 块
        long save = ftell(f);
        if (fseek(f, (long)pos, SEEK_SET) != 0) { fseek(f, save, SEEK_SET); continue; }
        uint8_t ch[36];
        if (fread(ch, 1, 36, f) != 36 || rd32(ch) != 36 || rd32(ch + 4) != type) {
            fseek(f, save, SEEK_SET);
            continue;
        }
        int32_t w = (int32_t)rd32(ch + 16), h = (int32_t)rd32(ch + 20);
        int32_t hx = (int32_t)rd32(ch + 24), hy = (int32_t)rd32(ch + 28);
        int32_t d = abs((int32_t)sub - want);
        if (w <= 0 || h <= 0 || w > 512 || h > 512 || (bestpx && d >= best)) {
            fseek(f, save, SEEK_SET);
            continue;
        }
        size_t bytes = (size_t)w * (size_t)h * 4;
        uint32_t *px = (uint32_t *)malloc(bytes);
        if (px && fread(px, 1, bytes, f) == bytes) {
            if (bestpx) free(bestpx);
            bestpx = px; best = d; bw = w; bh = h; bhx = hx; bhy = hy;
        } else if (px) {
            free(px);
        }
        fseek(f, save, SEEK_SET);
    }
    fclose(f);
    if (!bestpx) return false;
    *ow = bw; *oh = bh; *ohx = bhx; *ohy = bhy; *opx = bestpx;
    return true;
}

// 在 XCURSOR_PATH 里找 <theme>/cursors/<name>（先直名，再哈希名）
static bool xc_try(const char *dir, const char *theme, const char *name, int32_t want,
                   int32_t *w, int32_t *h, int32_t *hx, int32_t *hy, uint32_t **px) {
    char buf[700];
    snprintf(buf, sizeof buf, "%s/%s/cursors/%s", dir, theme, name);
    if (xc_load_file(buf, want, w, h, hx, hy, px)) return true;
    snprintf(buf, sizeof buf, "%s/%s/cursors/%08x", dir, theme, xc_hash(name));
    return xc_load_file(buf, want, w, h, hx, hy, px);
}

static bool xc_load(uint32_t shape, int32_t *w, int32_t *h, int32_t *hx, int32_t *hy,
                    uint32_t **px) {
    int32_t base = 24;
    const char *sz = getenv("XCURSOR_SIZE");
    if (sz && atoi(sz) > 0) base = atoi(sz);
    // 光标画在**物理**像素坐标系里：g_scale=1 时 24px 在 1272x2800 的屏上太小了。
    // 低于 2 倍统一按 2 倍（等效 48px），想更大用 XCURSOR_SIZE 覆盖。
    const int32_t eff = (g_scale > 1 ? g_scale : 2);
    const int32_t want = base * eff;

    // ⚠️ sfwm 由 root（su）拉起，su 经常把 XCURSOR_* 环境变量丢掉，HOME 也可能是
    //    "/" —— 所以除了环境变量，必须把 Termux 的固定路径硬编码兜底，
    //    否则永远找不到 ~/.icons/Bibata-Modern-Classic。
    char paths[768];
    int n = 0;
    const char *env = getenv("XCURSOR_PATH");
    const char *home = getenv("HOME");
    paths[0] = 0;
    if (env && *env) n += snprintf(paths + n, sizeof paths - n, "%s:", env);
    if (home && *home && n < (int)sizeof paths)
        n += snprintf(paths + n, sizeof paths - n, "%s/.icons:", home);
    if (n < (int)sizeof paths)
        snprintf(paths + n, sizeof paths - n,
                 "/data/data/com.termux/files/home/.icons:"
                 "/data/data/com.termux/files/usr/share/icons:/usr/share/icons");

    const char *themes[4];
    int nt = 0;
    const char *t1 = getenv("XCURSOR_THEME");
    if (t1 && *t1) themes[nt++] = t1;
    themes[nt++] = "Bibata-Modern-Classic";
    themes[nt++] = "default";
    themes[nt++] = "Adwaita";

    const char *nm = cursor_shape_name(shape);
    const char *tryn[2] = { nm, "left_ptr" };
    const int ntries = (strcmp(nm, "left_ptr") == 0) ? 1 : 2;

    for (int ti = 0; ti < nt; ti++) {
        for (int k = 0; k < ntries; k++) {
            const char *p = paths;
            while (p && *p) {
                const char *colon = strchr(p, ':');
                size_t len = colon ? (size_t)(colon - p) : strlen(p);
                if (len > 0 && len < 300) {
                    char dir[320];
                    memcpy(dir, p, len); dir[len] = 0;
                    if (xc_try(dir, themes[ti], tryn[k], want, w, h, hx, hy, px)) {
                        printf("[cursor] 命中 %s/%s/cursors/%s\n", dir, themes[ti], tryn[k]);
                        return true;
                    }
                }
                p = colon ? colon + 1 : nullptr;
            }
        }
    }
    printf("[cursor] 一条路径都没命中（shape=%u want=%d）paths=%s\n", shape, want, paths);
    return false;
}

// 双向箭头：dir 0=水平 1=垂直 2="/" 3="\"；thick 是线宽
static void cur_dbl_arrow(int dir, int32_t thick, uint32_t c) {
    const int32_t C = CUR_N / 2;
    const int32_t lo = 2, hi = CUR_N - 3;           // 两端
    const int32_t ah = 5;                            // 箭头半高
    if (dir == 0) {
        cur_rect(lo, C, hi, C + thick - 1, c);
        cur_poly((int32_t[]){lo, C, lo + ah, C - ah, lo + ah, C + ah}, 3, c);
        cur_poly((int32_t[]){hi, C, hi - ah, C - ah, hi - ah, C + ah}, 3, c);
    } else if (dir == 1) {
        cur_rect(C, lo, C + thick - 1, hi, c);
        cur_poly((int32_t[]){C, lo, C - ah, lo + ah, C + ah, lo + ah}, 3, c);
        cur_poly((int32_t[]){C, hi, C - ah, hi - ah, C + ah, hi - ah}, 3, c);
    } else {
        const int32_t a = 3, b = CUR_N - 4;
        if (dir == 2) cur_line(a, b, b, a, c); else cur_line(a, a, b, b, c);
        int32_t h1x = (dir == 2) ? a : a, h1y = (dir == 2) ? b : a;
        int32_t h2x = (dir == 2) ? b : b, h2y = (dir == 2) ? a : b;
        int32_t s = (dir == 2) ? -1 : 1;
        cur_poly((int32_t[]){h1x, h1y, h1x + ah, h1y, h1x, h1y + s * ah}, 3, c);
        cur_poly((int32_t[]){h2x, h2y, h2x - ah, h2y, h2x, h2y - s * ah}, 3, c);
    }
}

static void cursor_build(uint32_t shape) {
    if (g_cur_px) { free(g_cur_px); g_cur_px = nullptr; }
    g_cur_w = g_cur_h = 0;
    g_cur_hx = g_cur_hy = 0;
    g_cur_mul = 1;

    // ① 优先用真正的 X 光标主题（XCURSOR_THEME / XCURSOR_PATH / XCURSOR_SIZE）
    if (xc_load(shape, &g_cur_w, &g_cur_h, &g_cur_hx, &g_cur_hy, &g_cur_px)) {
        g_cur_mul = 1;
        g_cur_built = true;
        printf("[cursor] 主题图 %dx%d hot=(%d,%d) shape=%u\n",
               g_cur_w, g_cur_h, g_cur_hx, g_cur_hy, shape);
        return;
    }
    printf("[cursor] 主题里没找到 shape=%u，退回内置箭头\n", shape);

    // ② 兜底：程序化 24x24 箭头（主题缺失时不至于完全没光标）
    g_cur_w = g_cur_h = CUR_N;
    g_cur_px = (uint32_t *)calloc((size_t)CUR_N * CUR_N, 4);
    if (!g_cur_px) { g_cur_w = g_cur_h = 0; return; }
    for (int i = 0; i < CUR_N * CUR_N; i++) g_cur_px[i] = CUR_CLR;
    g_cur_mul = 0;      // 0 = 掉帧时按 g_scale 放大

    switch (shape) {
    case 9: case 10: {                     // text / vertical-text：I 形
        cur_rect(6, 2, 12, 4, CUR_OPQ);
        cur_rect(8, 2, 9, 20, CUR_OPQ);
        cur_rect(6, 18, 12, 20, CUR_OPQ);
        cur_rect(7, 3, 11, 3, CUR_WHT);
        cur_rect(9, 4, 9, 18, CUR_WHT);
        cur_rect(7, 19, 11, 19, CUR_WHT);
        g_cur_hx = 9; g_cur_hy = 11;
        break;
    }
    case 4: case 16: case 17: {            // pointer / grab / grabbing：手掌
        cur_disc(11, 15, 6, CUR_OPQ);
        cur_rect(4, 12, 17, 20, CUR_OPQ);
        cur_rect(9, 3, 12, 15, CUR_OPQ);   // 食指
        cur_disc(11, 15, 4, CUR_WHT);
        cur_rect(6, 13, 15, 19, CUR_WHT);
        cur_rect(10, 4, 11, 14, CUR_WHT);
        g_cur_hx = 10; g_cur_hy = 4;
        break;
    }
    case 7: case 8: {                      // cell / crosshair：准星
        cur_rect(2, 10, CUR_N - 3, 12, CUR_OPQ);
        cur_rect(10, 2, 12, CUR_N - 3, CUR_OPQ);
        cur_rect(3, 11, CUR_N - 4, 11, CUR_WHT);
        cur_rect(11, 3, 11, CUR_N - 4, CUR_WHT);
        g_cur_hx = 11; g_cur_hy = 11;
        break;
    }
    case 13: case 32: {                    // move / all-scroll：四向
        cur_dbl_arrow(0, 2, CUR_OPQ);
        cur_dbl_arrow(1, 2, CUR_OPQ);
        cur_dbl_arrow(0, 1, CUR_WHT);
        cur_dbl_arrow(1, 1, CUR_WHT);
        g_cur_hx = 11; g_cur_hy = 11;
        break;
    }
    case 14: case 15: {                    // no-drop / not-allowed
        cur_circle(11, 11, 9, 3, CUR_OPQ);
        cur_circle(11, 11, 9, 1, CUR_WHT);
        cur_circle(11, 11, 10, 1, CUR_WHT);
        cur_line(5, 5, 17, 17, CUR_OPQ);
        cur_line(5, 5, 17, 17, CUR_WHT);
        g_cur_hx = 11; g_cur_hy = 11;
        break;
    }
    case 5: case 6: {                      // progress / wait
        cur_circle(11, 11, 9, 2, CUR_OPQ);
        cur_circle(11, 11, 9, 1, CUR_WHT);
        cur_line(11, 4, 11, 11, CUR_WHT);
        g_cur_hx = 11; g_cur_hy = 11;
        break;
    }
    case 18: case 25: case 26: case 30: {  // e / w / ew / col-resize
        cur_dbl_arrow(0, 2, CUR_OPQ);
        cur_dbl_arrow(0, 1, CUR_WHT);
        g_cur_hx = 11; g_cur_hy = 11;
        break;
    }
    case 19: case 22: case 27: case 31: {  // n / s / ns / row-resize
        cur_dbl_arrow(1, 2, CUR_OPQ);
        cur_dbl_arrow(1, 1, CUR_WHT);
        g_cur_hx = 11; g_cur_hy = 11;
        break;
    }
    case 20: case 24: case 28: {           // ne / sw / nesw-resize  →  "/"
        cur_dbl_arrow(2, 2, CUR_OPQ);
        cur_dbl_arrow(2, 1, CUR_WHT);
        g_cur_hx = 11; g_cur_hy = 11;
        break;
    }
    case 21: case 23: case 29: {           // nw / se / nwse-resize  →  "\"
        cur_dbl_arrow(3, 2, CUR_OPQ);
        cur_dbl_arrow(3, 1, CUR_WHT);
        g_cur_hx = 11; g_cur_hy = 11;
        break;
    }
    case 33: case 34: {                    // zoom-in / zoom-out
        cur_circle(10, 10, 7, 2, CUR_OPQ);
        cur_circle(10, 10, 7, 1, CUR_WHT);
        cur_line(15, 15, 21, 21, CUR_OPQ);
        cur_line(15, 15, 21, 21, CUR_WHT);
        cur_line(6, 10, 14, 10, CUR_WHT);
        if (shape == 33) cur_line(10, 6, 10, 14, CUR_WHT);
        g_cur_hx = 10; g_cur_hy = 10;
        break;
    }
    default: {                             // 默认箭头
        cur_poly_ol((int32_t[]){0, 0, 14, 13, 8, 13, 11, 20, 8, 21, 5, 14, 0, 19}, 7);
        g_cur_hx = 0; g_cur_hy = 0;
        break;
    }
    }
    g_cur_built = true;
}

// 把光标贴到 (g_cur_x, g_cur_y)（物理坐标）
static void cursor_present(void) {
    if (g_have_client_cursor) {
        if (g_cur_layer.win) {
            g_cur_layer.hide();
            g_cur_lw = g_cur_lh = 0;
        }
        return;
    }
    if (!g_cur_built) cursor_build(g_cur_shape);
    if (!g_cur_px || g_cur_w <= 0 || g_cur_h <= 0) return;
    const int32_t mul = g_cur_mul > 0 ? g_cur_mul : (g_scale > 0 ? g_scale : 1);
    const int32_t pw = g_cur_w * mul, ph = g_cur_h * mul;

    if (!g_cur_layer.win || g_cur_lw != pw || g_cur_lh != ph) {
        if (g_cur_layer.win) g_cur_layer.hide();
        if (!g_cur_layer.create("sfwm-cursor", pw, ph)) return;
        g_cur_layer.cap_w = pw;
        g_cur_layer.cap_h = ph;
        g_cur_layer.setTouchable(false);
        // 关键：光标的 touchableRegion 已经是空的，但 Android 12+ 还会因为
        // touchOcclusionMode=BLOCK_UNTRUSTED 把光标那块像素下的触摸吞掉。
        // 必须显式声明“受信任覆盖层 + 丢弃所有输入”。
        if (!g_cur_layer.makeInputTransparent())
            printf("[cursor] 警告：makeInputTransparent 失败（缺 setTrustedOverlay/setDropInputMode）\n");
        ANativeWindow_setBuffersGeometry(g_cur_layer.win, pw, ph, WINDOW_FORMAT_RGBA_8888);
        g_cur_layer.setGeometry(sf::Rect{0, 0, pw, ph}, sf::Rect{0, 0, pw, ph});
        g_cur_lw = pw; g_cur_lh = ph;
    }
    // ⚠️ 每次都要设：sf::Layer::create() 会把 m_touchable 重置回 true，
    //    只在创建时设一次的话，指针一换尺寸（主题图 72x72 / 内置 24x24）光标层
    //    就重新变成“可触摸”，把下面的窗口点击吃掉。
    g_cur_layer.setTouchable(false);
    // 先 show + apply，ANativeWindow_lock 才有效（和 View::blit 里的顺序一致）
    g_cur_layer.place(g_cur_x - g_cur_hx * mul, g_cur_y - g_cur_hy * mul, g_base_z + 60000);

    ANativeWindow_Buffer ab;
    if (ANativeWindow_lock(g_cur_layer.win, &ab, nullptr) != 0) return;
    if (ab.bits && ab.stride > 0) {
        for (int32_t ly = 0; ly < g_cur_h; ly++) {
            for (int32_t lx = 0; lx < g_cur_w; lx++) {
                uint32_t c = g_cur_px[ly * g_cur_w + lx];
                // ARGB8888 -> RGBA8888（little endian 内存序 B,G,R,A）
                uint8_t b = (uint8_t)(c & 0xFF), gg = (uint8_t)((c >> 8) & 0xFF);
                uint8_t r = (uint8_t)((c >> 16) & 0xFF), a = (uint8_t)((c >> 24) & 0xFF);
                for (int32_t sy2 = 0; sy2 < mul; sy2++) {
                    int32_t py = ly * mul + sy2;
                    if (py >= ab.height) break;
                    uint8_t *row = (uint8_t *)ab.bits + (size_t)py * (size_t)ab.stride * 4;
                    for (int32_t sx2 = 0; sx2 < mul; sx2++) {
                        int32_t px = lx * mul + sx2;
                        if (px >= ab.width) break;
                        uint8_t *d = row + (size_t)px * 4;
                        d[0] = b; d[1] = gg; d[2] = r; d[3] = a;
                    }
                }
            }
        }
    }
    ANativeWindow_unlockAndPost(g_cur_layer.win);
    g_cur_shown = true;
}

static void cursor_hide(void) {
    if (g_cur_shown && g_cur_layer.win) {
        g_cur_layer.hide();
        g_cur_shown = false;
    }
}

static uint32_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// ---------------------------------------------------------------- View

// ── 合成测试图案：用来判定闪烁出在“上传路径”还是“客户端内容”
//    SFWM_TESTPAT=1 时，每帧在图层缓冲上盖一层静态色块：
//      顶部四块 红/绿/蓝/白 —— 完全静态，闪 = SurfaceFlinger 上传/合成路径有问题
//      中间一条渐变细线 —— 有撕裂会看出来
//      右下角滴答块 —— 每 post 一次换色，用来看更新节奏
static int testpat_mode() {
    static int m = -2;
    if (m == -2) { const char *e = getenv("SFWM_TESTPAT"); m = e ? atoi(e) : 0; }
    return m;
}
static uint32_t g_tp_tick = 0;

// 图层是 BGRA_8888：内存 B,G,R,A → 小端 uint32 = A<<24 | R<<16 | G<<8 | B
static inline uint32_t tp_px(uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void draw_testpat(ANativeWindow_Buffer &ab, uint32_t tick) {
    if (!ab.bits || ab.width <= 2 || ab.height <= 2 || ab.stride <= 0) return;
    uint32_t *base = (uint32_t *)ab.bits;
    const int W = ab.width, H = ab.height, st = ab.stride;
    const uint32_t cols[4] = { tp_px(255, 0, 0), tp_px(0, 255, 0),
                               tp_px(0, 0, 255), tp_px(255, 255, 255) };
    const int bw = W / 4, bh = H / 6;
    for (int c = 0; c < 4; c++)
        for (int y = 0; y < bh; y++) {
            uint32_t *row = base + (size_t)y * st + (size_t)c * bw;
            for (int x = 0; x < bw && c * bw + x < W; x++) row[x] = cols[c];
        }
    for (int x = 0; x < W; x++) {
        const int t = x * 255 / (W ? W : 1);
        base[(size_t)(H / 2) * st + x] = tp_px((uint8_t)t, 128, (uint8_t)(255 - t));
    }
    const uint32_t tc = (tick & 1) ? tp_px(255, 0, 255) : tp_px(255, 255, 0);
    for (int y = H - H / 8; y < H; y++) {
        uint32_t *row = base + (size_t)y * st + (size_t)(W - W / 8);
        for (int x = 0; x < W / 8; x++) row[x] = tc;
    }
}

struct View;
static void hold_release(View *v);

#include "view.hpp"

// ---------------------------------------------------------------- wl_region
static void region_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void region_add(wl_client *, wl_resource *, int32_t, int32_t, int32_t, int32_t) {}
static void region_subtract(wl_client *, wl_resource *, int32_t, int32_t, int32_t, int32_t) {}
static const struct wl_region_interface region_impl = {
    .destroy = region_destroy,
    .add = region_add,
    .subtract = region_subtract,
};

static void region_handle_destroyed(wl_resource *r) { (void)r; }

// set_opaque_region / set_input_region 不转移所有权！客户端会自己 destroy，
// 服务端提前销毁会导致客户端后续请求 → wl_display invalid_object。
static void surface_set_region(wl_client *, wl_resource *, wl_resource *region, bool input) {
    (void)region;
    (void)input;
}

// ---------------------------------------------------------------- wl_surface
static void surf_destroy(wl_client *, wl_resource *r) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (v && v->read_next) {          // 还攥着的帧必须还给客户端
        if (v->hold) { wl_list_remove(&v->hold->lis.link); delete v->hold; v->hold = nullptr; }
        wl_buffer_send_release(v->read_next);
        v->read_next = nullptr;
    }
    wl_resource_destroy(r);
}

static void surf_attach(wl_client *, wl_resource *r, wl_resource *buffer, int32_t, int32_t) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (v) v->pending_buffer = buffer;
}

static void surf_damage(wl_client *, wl_resource *, int32_t, int32_t, int32_t, int32_t) {}

static void surf_frame(wl_client *, wl_resource *r, uint32_t id) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    wl_resource *cb = wl_resource_create(wl_resource_get_client(r), &wl_callback_interface,
                                         wl_resource_get_version(r), id);
    if (!cb) { wl_client_post_no_memory(wl_resource_get_client(r)); return; }
    if (v->frame_cb) wl_resource_destroy(v->frame_cb);
    v->frame_cb = cb;
}

static void surf_set_opaque_region(wl_client *c, wl_resource *r, wl_resource *region) {
    surface_set_region(c, r, region, false);
}
static void surf_set_input_region(wl_client *c, wl_resource *r, wl_resource *region) {
    surface_set_region(c, r, region, true);
}

static void surf_commit(wl_client *, wl_resource *r) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (v) v->commit();
}

static void surf_set_buffer_transform(wl_client *, wl_resource *, int32_t) {}
static void surf_set_buffer_scale(wl_client *, wl_resource *r, int32_t scale) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    v->buf_scale = scale > 1 ? scale : 1;
    printf("[input] buffer_scale=%d\n", v->buf_scale);
}
static void surf_damage_buffer(wl_client *, wl_resource *, int32_t, int32_t, int32_t, int32_t) {}

// wl_surface.offset (v5)：用于光标表面等需要“只改位置不改内容”的场景
static void surf_offset(wl_client *, wl_resource *r, int32_t dx, int32_t dy) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v || (dx == 0 && dy == 0)) return;
    v->x += dx;
    v->y += dy;
    if (v->mapped) v->place();
}

static const struct wl_surface_interface surface_impl = {
    .destroy = surf_destroy,
    .attach = surf_attach,
    .damage = surf_damage,
    .frame = surf_frame,
    .set_opaque_region = surf_set_opaque_region,
    .set_input_region = surf_set_input_region,
    .commit = surf_commit,
    .set_buffer_transform = surf_set_buffer_transform,
    .set_buffer_scale = surf_set_buffer_scale,
    .damage_buffer = surf_damage_buffer,
    .offset = surf_offset,
};

// ---- wl_shm 服务端实现
static void shm_buffer_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct wl_buffer_interface buffer_impl = {
    .destroy = shm_buffer_destroy,
};

static void shm_pool_create_buffer(wl_client *c, wl_resource *r, uint32_t id, int32_t offset,
                                   int32_t width, int32_t height, int32_t stride, uint32_t format) {
    ShmPool *pool = (ShmPool *)wl_resource_get_user_data(r);
    if (!pool || !pool->base) { wl_client_post_implementation_error(c, "pool 未映射"); return; }

    if (width <= 0 || height <= 0 || stride < width * 4) {
        wl_resource_post_error(r, WL_SHM_ERROR_INVALID_STRIDE, "参数非法");
        return;
    }
    int64_t need = (int64_t)offset + (int64_t)stride * height;
    if (offset < 0 || need > (int64_t)pool->size) {
        wl_resource_post_error(r, WL_SHM_ERROR_INVALID_FD, "缓冲越界 %lld > %d",
                               (long long)need, pool->size);
        return;
    }

    wl_resource *br = wl_resource_create(c, &wl_buffer_interface, 1, id);
    if (!br) { wl_client_post_no_memory(c); return; }
    ShmBuf *b = new ShmBuf();
    b->pool = pool;
    b->offset = offset;
    pool->refs++;              // buffer 活多久，映射就留多久
    b->w = width; b->h = height; b->stride = stride; b->format = format;
    wl_resource_set_implementation(br, &buffer_impl, b, [](wl_resource *res) {
        ShmBuf *bb = (ShmBuf *)wl_resource_get_user_data(res);
        if (!bb) return;
        shm_pool_unref(bb->pool);
        delete bb;
    });
}

static void shm_pool_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static void shm_pool_resize(wl_client *, wl_resource *r, int32_t size) {
    ShmPool *pool = (ShmPool *)wl_resource_get_user_data(r);
    if (!pool || size <= pool->size) return;
    if (pool->base) munmap(pool->base, pool->size);
    void *p = mmap(nullptr, size, PROT_READ, MAP_SHARED, pool->fd, 0);
    if (p == MAP_FAILED) { pool->base = nullptr; wl_resource_post_no_memory(r); return; }
    pool->base = (uint8_t *)p;
    pool->size = size;
}

static const struct wl_shm_pool_interface shm_pool_impl = {
    .create_buffer = shm_pool_create_buffer,
    .destroy = shm_pool_destroy,
    .resize = shm_pool_resize,
};

static void shm_create_pool(wl_client *c, wl_resource *r, uint32_t id, int32_t fd, int32_t size) {
    void *p = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        wl_resource_post_error(r, WL_SHM_ERROR_INVALID_FD, "mmap 失败: %s", strerror(errno));
        close(fd);
        return;
    }
    wl_resource *pres = wl_resource_create(c, &wl_shm_pool_interface, 1, id);
    if (!pres) { munmap(p, size); close(fd); wl_client_post_no_memory(c); return; }

    ShmPool *pool = new ShmPool();
    pool->fd = fd; pool->size = size; pool->base = (uint8_t *)p;
    wl_resource_set_implementation(pres, &shm_pool_impl, pool, [](wl_resource *res) {
        shm_pool_unref((ShmPool *)wl_resource_get_user_data(res));
    });
}

static void shm_release(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct wl_shm_interface shm_impl = {
    .create_pool = shm_create_pool,
    .release = shm_release,
};

static void bind_shm(wl_client *c, void *, uint32_t version, uint32_t id) {
    wl_resource *r = wl_resource_create(c, &wl_shm_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &shm_impl, nullptr, nullptr);
    wl_shm_send_format(r, WL_SHM_FORMAT_ARGB8888);
    wl_shm_send_format(r, WL_SHM_FORMAT_XRGB8888);
}

// ---------------------------------------------------------------- zwp_linux_dmabuf_v1
// Mesa/zink 在 Wayland 上要求合成器支持 wl_drm 或 zwp_linux_dmabuf_v1，否则
// egl_dri2 会断言 "software || dri2_dpy->fd_render_gpu >= 0" 而直接挂掉。
// 这里提供 dmabuf 协议；真正的 buffer 导入（送进 SF）在下一步做。

struct DmabufParams {
    DmabufBuf buf;
};

static void dmabuf_buffer_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct wl_buffer_interface dmabuf_buffer_impl = {
    .destroy = dmabuf_buffer_destroy,
};

static void dmabuf_params_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static void dmabuf_params_add(wl_client *, wl_resource *r, int32_t fd, uint32_t plane_idx,
                              uint32_t offset, uint32_t stride, uint32_t modifier_hi,
                              uint32_t modifier_lo) {
    DmabufParams *p = (DmabufParams *)wl_resource_get_user_data(r);
    if (!p) { close(fd); return; }
    if (plane_idx >= 4) {
        wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX,
                               "plane_idx %u 太大", plane_idx);
        close(fd);
        return;
    }
    if (p->buf.fds[plane_idx] >= 0) close(p->buf.fds[plane_idx]);
    p->buf.fds[plane_idx] = fd;
    p->buf.offset[plane_idx] = offset;
    p->buf.stride[plane_idx] = stride;
    p->buf.modifier = ((uint64_t)modifier_hi << 32) | modifier_lo;
    if ((int)plane_idx + 1 > p->buf.nplanes) p->buf.nplanes = (int)plane_idx + 1;
}

static wl_resource *dmabuf_make_buffer(wl_client *c, DmabufParams *p, int32_t width,
                                       int32_t height, uint32_t format, uint32_t buffer_id) {
    if (width <= 0 || height <= 0 || p->buf.nplanes == 0) {
        return nullptr;
    }
    wl_resource *br = wl_resource_create(c, &wl_buffer_interface, 1, buffer_id);
    if (!br) { wl_client_post_no_memory(c); return nullptr; }

    DmabufBuf *b = new DmabufBuf();
    *b = p->buf;              // fd 所有权转给 buffer
    b->w = width;
    b->h = height;
    b->drm_format = format;
    for (int i = 0; i < 4; i++) p->buf.fds[i] = -1;
    p->buf.nplanes = 0;

    wl_resource_set_implementation(br, &dmabuf_buffer_impl, b, [](wl_resource *res) {
        DmabufBuf *bb = (DmabufBuf *)wl_resource_get_user_data(res);
        if (!bb) return;
        for (int i = 0; i < 4; i++) if (bb->fds[i] >= 0) close(bb->fds[i]);
        delete bb;
    });
    return br;
}

// 异步版：buffer 的资源由服务端分配（id=0 → wayland 自动分配），通过 created 事件送出
static void dmabuf_params_create(wl_client *c, wl_resource *r, int32_t width, int32_t height,
                                 uint32_t format, uint32_t flags) {
    (void)flags;
    DmabufParams *p = (DmabufParams *)wl_resource_get_user_data(r);
    if (!p) return;
    if (width <= 0 || height <= 0 || p->buf.nplanes == 0) {
        wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
                               "参数不全 %dx%d planes=%d", width, height, p->buf.nplanes);
        return;
    }
    wl_resource *br = dmabuf_make_buffer(c, p, width, height, format, 0);
    if (!br) return;
    zwp_linux_buffer_params_v1_send_created(r, br);
}

// 同步版（v2+）：客户端自己给 buffer id（注意 create_immed 的 buffer_id 在最前面）
static void dmabuf_params_create_immed(wl_client *c, wl_resource *r, uint32_t buffer_id,
                                       int32_t width, int32_t height,
                                       uint32_t format, uint32_t flags) {
    (void)flags;
    DmabufParams *p = (DmabufParams *)wl_resource_get_user_data(r);
    if (!p) return;
    if (!dmabuf_make_buffer(c, p, width, height, format, buffer_id)) {
        wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
                               "参数不全 %dx%d planes=%d", width, height, p->buf.nplanes);
    }
}

static const struct zwp_linux_buffer_params_v1_interface dmabuf_params_impl = {
    .destroy = dmabuf_params_destroy,
    .add = dmabuf_params_add,
    .create = dmabuf_params_create,
    .create_immed = dmabuf_params_create_immed,
};

static void dmabuf_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static void dmabuf_create_params(wl_client *c, wl_resource *r, uint32_t id) {
    wl_resource *pr = wl_resource_create(c, &zwp_linux_buffer_params_v1_interface,
                                        wl_resource_get_version(r), id);
    if (!pr) { wl_client_post_no_memory(c); return; }
    DmabufParams *p = new DmabufParams();
    wl_resource_set_implementation(pr, &dmabuf_params_impl, p, [](wl_resource *res) {
        DmabufParams *pp = (DmabufParams *)wl_resource_get_user_data(res);
        if (!pp) return;
        for (int i = 0; i < 4; i++) if (pp->buf.fds[i] >= 0) close(pp->buf.fds[i]);
        delete pp;
    });
}

// v4 新增的 surface feedback：我们不做直接扫描输出，但必须把格式表/设备告诉客户端，
// 否则嵌套窗管（aquamarine）拿不到 allocator。
static void dmabuf_feedback_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct zwp_linux_dmabuf_feedback_v1_interface dmabuf_feedback_impl = {
    .destroy = dmabuf_feedback_destroy,
};

static void dmabuf_send_feedback(wl_resource *f) {
    // 格式表：每项 format(u32) + pad(u32) + modifier(u64)
    struct __attribute__((packed)) Entry { uint32_t format, pad; uint64_t mod; };
    Entry entries[2] = { { 0x34325241u, 0, 0 }, { 0x34325258u, 0, 0 } };
    const uint32_t sz = (uint32_t)sizeof(entries);
    int fd = sfwm_rt_file("sfwm-dmabuf-fmt", 0);
    if (fd < 0) return;
    if (write(fd, entries, sz) != (ssize_t)sz) { close(fd); return; }
    lseek(fd, 0, SEEK_SET);
    zwp_linux_dmabuf_feedback_v1_send_format_table(f, fd, sz);
    close(fd);

    struct stat st;
    uint64_t dev = 0;
    if (stat("/dev/dri/renderD128", &st) == 0) dev = (uint64_t)st.st_rdev;

    struct wl_array devarr;
    wl_array_init(&devarr);
    uint32_t *dv = (uint32_t *)wl_array_add(&devarr, 2 * sizeof(uint32_t));
    if (dv) { dv[0] = (uint32_t)(dev >> 32); dv[1] = (uint32_t)dev; }
    zwp_linux_dmabuf_feedback_v1_send_main_device(f, &devarr);
    zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(f, &devarr);
    wl_array_release(&devarr);

    zwp_linux_dmabuf_feedback_v1_send_tranche_flags(f, 0 /* SCANOUT 不开 */);

    struct wl_array idx;
    wl_array_init(&idx);
    uint16_t *iv = (uint16_t *)wl_array_add(&idx, 2 * sizeof(uint16_t));
    if (iv) { iv[0] = 0; iv[1] = 1; }
    zwp_linux_dmabuf_feedback_v1_send_tranche_formats(f, &idx);
    wl_array_release(&idx);

    zwp_linux_dmabuf_feedback_v1_send_tranche_done(f);
    zwp_linux_dmabuf_feedback_v1_send_done(f);
}

static void dmabuf_make_feedback(wl_client *c, wl_resource *res, uint32_t id) {
    wl_resource *f = wl_resource_create(c, &zwp_linux_dmabuf_feedback_v1_interface,
                                       wl_resource_get_version(res), id);
    if (!f) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(f, &dmabuf_feedback_impl, nullptr, nullptr);
    dmabuf_send_feedback(f);
}

static void dmabuf_get_default_feedback(wl_client *c, wl_resource *res, uint32_t id) {
    dmabuf_make_feedback(c, res, id);
}

static void dmabuf_get_surface_feedback(wl_client *c, wl_resource *res, uint32_t id, wl_resource *) {
    dmabuf_make_feedback(c, res, id);
}

static const struct zwp_linux_dmabuf_v1_interface dmabuf_impl = {
    .destroy = dmabuf_destroy,
    .create_params = dmabuf_create_params,
    .get_default_feedback = dmabuf_get_default_feedback,
    .get_surface_feedback = dmabuf_get_surface_feedback,
};

static void bind_dmabuf(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 4) version = 4;
    printf("[sfwm] 客户端绑定了 zwp_linux_dmabuf_v1 (请求 v%u)\n", version);
    wl_resource *r = wl_resource_create(c, &zwp_linux_dmabuf_v1_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &dmabuf_impl, nullptr, nullptr);
    const uint32_t fmts[2] = { 0x34325241u /*ARGB8888*/, 0x34325258u /*XRGB8888*/ };
    for (int i = 0; i < 2; i++) {
        if (version >= 3) {
            // v3 起用 modifiers 事件；只广告 LINEAR，够客户端分配用
            zwp_linux_dmabuf_v1_send_modifier(r, fmts[i], 0, 0);
        } else {
            wl_resource_post_event(r, ZWP_LINUX_DMABUF_V1_FORMAT, fmts[i]);
        }
    }
}

// ---------------------------------------------------------------- wl_drm
// Mesa 的 wayland EGL 在 drm_handle_device() 里会：
//   device_name = wl_drm.device 事件给的字符串
//   fd_render_gpu = open(device_name)        ← 我们要把 /dev/kgsl-3d0 给它
// 然后 freedreno 的 fd_device_new(fd) 看 drmGetVersion()->name：
//   "msm" → 上游 DRM 后端；"kgsl" → KGSL 后端（这台机器的 GPU 就在 kgsl 后面）
// 这正是硬件加速能跑起来的关键：默认它只会去开 /dev/dri/renderD128（显示器节点）。
static const char *g_drm_device = "/dev/kgsl-3d0";

static void drm_authenticate_cb(wl_client *, wl_resource *r, uint32_t id) {
    printf("[sfwm] wl_drm 认证请求 id=%u → 直接放行\n", id);
    wl_drm_send_authenticated(r);   // 这个版本的协议 authenticated 事件不带参数
}

static void drm_create_buffer_cb(wl_client *c, wl_resource *r, uint32_t id, uint32_t name,
                                 int32_t width, int32_t height, uint32_t stride,
                                 uint32_t format) {
    printf("[sfwm] wl_drm.create_buffer %dx%d name=%u（不支持，回错误）\n",
           width, height, name);
    wl_resource_post_error(r, WL_DRM_ERROR_INVALID_NAME, "不支持 name 缓冲区，请用 dmabuf");
    (void)c; (void)id; (void)stride; (void)format;
}

static void drm_create_planar_buffer_cb(wl_client *, wl_resource *r, uint32_t id, uint32_t name,
                                        int32_t width, int32_t height, uint32_t format,
                                        int32_t offset0, int32_t stride0,
                                        int32_t offset1, int32_t stride1,
                                        int32_t offset2, int32_t stride2) {
    wl_resource_post_error(r, WL_DRM_ERROR_INVALID_NAME, "不支持 planar name 缓冲区");
    (void)id; (void)name; (void)width; (void)height; (void)format;
    (void)offset0; (void)stride0; (void)offset1; (void)stride1; (void)offset2; (void)stride2;
}

static const struct wl_drm_interface drm_impl = {
    .authenticate = drm_authenticate_cb,
    .create_buffer = drm_create_buffer_cb,
    .create_planar_buffer = drm_create_planar_buffer_cb,
};

static void bind_drm(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 2) version = 2;
    wl_resource *r = wl_resource_create(c, &wl_drm_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &drm_impl, nullptr, nullptr);

    const char *dev = getenv("SFWM_DRM_DEVICE");
    if (dev && *dev) g_drm_device = dev;
    printf("[sfwm] wl_drm 绑定，告知设备 %s\n", g_drm_device);
    wl_drm_send_device(r, g_drm_device);
    wl_drm_send_format(r, WL_DRM_FORMAT_ARGB8888);
    wl_drm_send_format(r, WL_DRM_FORMAT_XRGB8888);
    wl_drm_send_capabilities(r, WL_DRM_CAPABILITY_PRIME);
}

// ---------------------------------------------------------------- wl_compositor
static View *view_from_surface(wl_resource *r) { return (View *)wl_resource_get_user_data(r); }

static void compositor_create_surface(wl_client *c, wl_resource *res, uint32_t id) {
    View *v = new View();
    v->srv = &g_srv;
    v->z = g_srv.next_z++;
    v->x = (int32_t)(40 * (v->z % 5));
    v->y = (int32_t)(40 * (v->z % 5));

    wl_resource *sr = wl_resource_create(c, &wl_surface_interface, wl_resource_get_version(res), id);
    if (!sr) { delete v; wl_client_post_no_memory(c); return; }
    v->surface = sr;
    wl_resource_set_implementation(sr, &surface_impl, v, [](wl_resource *r) {
        View *vv = view_from_surface(r);
        if (!vv) return;
        for (size_t i = 0; i < g_srv.views.size(); i++) {
            if (g_srv.views[i] == vv) { g_srv.views.erase(g_srv.views.begin() + i); break; }
        }
        // 其它协议对象还挂着这个 View，先断开，避免悬空指针
        if (vv->xdg_surface) wl_resource_set_user_data(vv->xdg_surface, nullptr);
        if (vv->toplevel) wl_resource_set_user_data(vv->toplevel, nullptr);
        if (vv->layer_surface) wl_resource_set_user_data(vv->layer_surface, nullptr);
        vv->xdg_surface = nullptr;
        vv->toplevel = nullptr;
        vv->layer_surface = nullptr;
        // 从父表面的子列表里摘掉（子表面或父表面任意一方先销毁都要清）
        if (vv->sub_parent) {
            auto &s = vv->sub_parent->subs;
            for (size_t i = 0; i < s.size(); i++) if (s[i] == vv) { s.erase(s.begin() + i); break; }
            vv->sub_parent = nullptr;
        }
        vv->subs.clear();
        // 焦点指向的 View 没了 → 立刻清掉，否则后面拿野指针发 enter/leave
        if (g_srv.kb_focus == vv) g_srv.kb_focus = nullptr;
        if (g_srv.pointer_focus == vv) g_srv.pointer_focus = nullptr;
        if (g_srv.cursor_view == vv) g_srv.cursor_view = nullptr;
        if (vv->layer.win) vv->layer.hide();
        delete vv;
    });
    g_srv.views.push_back(v);
}

static void compositor_create_region(wl_client *c, wl_resource *res, uint32_t id) {
    wl_resource *r = wl_resource_create(c, &wl_region_interface, wl_resource_get_version(res), id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &region_impl, nullptr, region_handle_destroyed);
}

static const struct wl_compositor_interface compositor_impl = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
};

static void bind_compositor(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 6) version = 6;   // v5 => wl_surface.offset；v6 只加了事件，没有新请求
    wl_resource *r = wl_resource_create(c, &wl_compositor_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &compositor_impl, nullptr, nullptr);
}

// ---------------------------------------------------------------- wl_subcompositor
// OBS 的预览区（program/preview monitor）不是普通 Qt 控件，而是一条独立子系统表面：
// 客户端在它上面建 wl_egl_window 自己用 GL 渲染，再附到某个 wl_surface 的 sub-surface 上。
// 没有这个全局对象，OBS 会报 "Can't create subsurface, not supported by the compositor"，
// 然后整个预览区永远是黑的。
struct SubsurfData { View *v = nullptr; };

static void sub_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static void sub_set_position(wl_client *, wl_resource *r, int32_t x, int32_t y) {
    SubsurfData *d = (SubsurfData *)wl_resource_get_user_data(r);
    if (!d || !d->v) return;
    d->v->sub_x = x;
    d->v->sub_y = y;
    if (d->v->mapped) d->v->place();
}

// 一律当成“在父之上”，OBS 只有一条预览子系统，不需要真正的兄弟排序
static void sub_place_above(wl_client *, wl_resource *, wl_resource *) {}
static void sub_place_below(wl_client *, wl_resource *, wl_resource *) {}

// sync 模式需要父提交时才生效；我们直接立即生效（desync 语义），
// 对单个预览表面没有撕裂风险，但少一层状态机。
static void sub_set_sync(wl_client *, wl_resource *) {}
static void sub_set_desync(wl_client *, wl_resource *) {}

static const struct wl_subsurface_interface subsurface_impl = {
    .destroy = sub_destroy,
    .set_position = sub_set_position,
    .place_above = sub_place_above,
    .place_below = sub_place_below,
    .set_sync = sub_set_sync,
    .set_desync = sub_set_desync,
};

static void subcompositor_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static void subcompositor_get_subsurface(wl_client *c, wl_resource *res, uint32_t id,
                                         wl_resource *surface, wl_resource *parent) {
    View *v = (View *)wl_resource_get_user_data(surface);
    View *p = (View *)wl_resource_get_user_data(parent);
    if (!v || !p || v == p) {
        wl_resource_post_error(res, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE, "子表面参数非法");
        return;
    }
    wl_resource *r = wl_resource_create(c, &wl_subsurface_interface,
                                        wl_resource_get_version(res), id);
    if (!r) { wl_client_post_no_memory(c); return; }

    v->is_subsurface = true;
    v->sub_parent = p;
    v->z = p->z;
    p->subs.push_back(v);

    SubsurfData *d = new SubsurfData();
    d->v = v;
    wl_resource_set_implementation(r, &subsurface_impl, d, [](wl_resource *res2) {
        SubsurfData *dd = (SubsurfData *)wl_resource_get_user_data(res2);
        if (dd && dd->v) {
            View *vv = dd->v;
            vv->is_subsurface = false;
            if (vv->sub_parent) {
                auto &s = vv->sub_parent->subs;
                for (size_t i = 0; i < s.size(); i++)
                    if (s[i] == vv) { s.erase(s.begin() + i); break; }
                vv->sub_parent = nullptr;
            }
        }
        delete dd;
    });
    printf("[sfwm] 建立 subsurface（预览表面）\n");
}

static const struct wl_subcompositor_interface subcompositor_impl = {
    .destroy = subcompositor_destroy,
    .get_subsurface = subcompositor_get_subsurface,
};

static void bind_subcompositor(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 1) version = 1;
    wl_resource *r = wl_resource_create(c, &wl_subcompositor_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &subcompositor_impl, nullptr, nullptr);
}

// ---------------------------------------------------------------- wl_output
static void output_release(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct wl_output_interface output_impl = {
    .release = output_release,
};

static void bind_output(wl_client *c, void *, uint32_t version, uint32_t id) {
    wl_resource *r = wl_resource_create(c, &wl_output_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &output_impl, nullptr, nullptr);

    // ★ wl_output 的物理尺寸(mm) 决定客户端算出来的 DPI。
    //   原来写死 340x750mm → Qt 算出 ≈95 DPI（“桌面”尺寸），
    //   但本机是 475ppi 的 6.8" 屏，95 DPI 的字在物理上只有 0.85mm 高，小得看不见。
    //   用 SFWM_DPI 指定想要的逻辑 DPI，mm 由像素数反推：
    //     SFWM_DPI=96   桌面观感（默认，等于原来的 340x750mm）
    //     SFWM_DPI=140  ≈1.45 倍字
    //     SFWM_DPI=168  ≈1.75 倍字（但 widget 最小尺寸也会变大，窗口可能装不下）
    {
        int32_t dpi = 0;
        if (const char *e = getenv("SFWM_DPI")) dpi = atoi(e);
        if (dpi <= 0) dpi = 168;   // 默认 168：字≈1.75 倍（实测 OBS 仍刚好装下 1272 宽），SFWM_DPI 可覆盖
        if (dpi > 400) dpi = 400;
        int32_t mm_w = (int32_t)((int64_t)g_out_w * 254 / ((int64_t)dpi * 10));
        int32_t mm_h = (int32_t)((int64_t)g_out_h * 254 / ((int64_t)dpi * 10));
        if (mm_w < 1) mm_w = 1;
        if (mm_h < 1) mm_h = 1;
        printf("[out] 上报物理尺寸 %dx%d mm（DPR=%d，屏幕 %dx%d）\n",
               mm_w, mm_h, dpi, g_out_w, g_out_h);
        wl_output_send_geometry(r, 0, 0, mm_w, mm_h, WL_OUTPUT_SUBPIXEL_UNKNOWN,
                                "sfwm", "SurfaceFlinger", WL_OUTPUT_TRANSFORM_NORMAL);
    }
    wl_output_send_mode(r, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                        g_out_w, g_out_h, 60000);
    if (version >= 2) wl_output_send_scale(r, g_scale);
    if (version >= 4) {
        wl_output_send_name(r, "SFWM-1");
        wl_output_send_description(r, "sfwm (SurfaceFlinger 图层输出)");
    }
    if (version >= 2) wl_output_send_done(r);
}

// ---------------------------------------------------------------- wl_seat / 输入
static void ptr_set_cursor(wl_client *c, wl_resource *, uint32_t, wl_resource *surface, int32_t hx, int32_t hy) {
    (void)c;
    if (!surface) { g_srv.cursor_view = nullptr; return; }
    View *v = (View *)wl_resource_get_user_data(surface);
    g_srv.cursor_view = v;
    g_srv.cursor_hot_x = hx;
    g_srv.cursor_hot_y = hy;
    if (v) v->is_cursor = true;
}
static void ptr_destroyed(wl_resource *r) {
    auto &v = g_srv.pointers;
    for (size_t i = 0; i < v.size(); i++)
        if (v[i] == r) { v.erase(v.begin() + i); printf("[input] pointer 销毁，剩 %zu 个\n", v.size()); break; }
}
// 注意：resource 的销毁路径有两条 —— 客户端发 release 请求，或者客户端直接断连。
// 只管 release 不够，断连时 vector 里会留下悬空指针，之后往它发事件就是
// “interface 'xdg_surface' has no event 5” 那类协议错误（对方直接断开）。
static void ptr_release(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct wl_pointer_interface pointer_impl = {
    .set_cursor = ptr_set_cursor,
    .release = ptr_release,
};

static void touch_destroyed(wl_resource *r) {
    auto &v = g_srv.touches;
    for (size_t i = 0; i < v.size(); i++) if (v[i] == r) { v.erase(v.begin() + i); break; }
}
static void touch_release(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct wl_touch_interface touch_impl = {
    .release = touch_release,
};

static bool ptr_same_client(wl_resource *p, View *v);

static void seat_get_pointer(wl_client *c, wl_resource *seat, uint32_t id) {
    wl_resource *r = wl_resource_create(c, &wl_pointer_interface, wl_resource_get_version(seat), id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &pointer_impl, nullptr, ptr_destroyed);
    g_srv.pointers.push_back(r);
    printf("[input] 新 pointer (v%u)，共 %zu 个\n", wl_resource_get_version(r), g_srv.pointers.size());

    // 如果焦点已经存在，必须给新 pointer 补一个 enter，否则客户端永远不认这个指针
    View *v = g_srv.pointer_focus;
    if (v && v->surface && ptr_same_client(r, v)) {
        uint32_t serial = wl_display_next_serial(g_srv.dpy);
        wl_pointer_send_enter(r, serial, v->surface,
                              wl_fixed_from_int((g_srv.pointer_x - v->x) / v->buf_scale),
                              wl_fixed_from_int((g_srv.pointer_y - v->y) / v->buf_scale));
        if (wl_resource_get_version(r) >= 5) wl_pointer_send_frame(r);
        printf("[input] 新 pointer 补发 enter -> (%d,%d)\n",
               g_srv.pointer_x - v->x, g_srv.pointer_y - v->y);
    }
}

static void seat_get_keyboard(wl_client *, wl_resource *, uint32_t);
static void seat_get_touch(wl_client *c, wl_resource *seat, uint32_t id) {
    wl_resource *r = wl_resource_create(c, &wl_touch_interface, wl_resource_get_version(seat), id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &touch_impl, nullptr, touch_destroyed);
    g_srv.touches.push_back(r);
}
static void seat_release(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct wl_seat_interface seat_impl = {
    .get_pointer = seat_get_pointer,
    .get_keyboard = seat_get_keyboard,
    .get_touch = seat_get_touch,
    .release = seat_release,
};

static bool g_have_input = false;
static bool g_touch_enabled = false;   // 默认只发 pointer：Qt Widgets 不处理 QTouchEvent，
                                       // 收到 wl_touch 反而会把点击吃掉。--touch 才开。

static void bind_seat(wl_client *c, void *, uint32_t version, uint32_t id) {
    wl_resource *r = wl_resource_create(c, &wl_seat_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &seat_impl, nullptr, nullptr);
    // 键盘不依赖触摸设备：走注入 socket，所以总是宣告
    uint32_t caps = WL_SEAT_CAPABILITY_KEYBOARD;
    if (g_have_input) {
        caps |= WL_SEAT_CAPABILITY_POINTER;
        if (g_touch_enabled) caps |= WL_SEAT_CAPABILITY_TOUCH;
    }
    wl_seat_send_capabilities(r, caps);
    if (version >= 2) wl_seat_send_name(r, "seat0");
}

// ---------------------------------------------------------------- evdev 触摸
struct MtSlot { int32_t id = -1; int32_t x = 0, y = 0; };

struct InputDev {
    int fd = -1;
    int32_t xmin = 0, xmax = 0, ymin = 0, ymax = 0;
    MtSlot slots[10];
    int32_t cur_slot = 0;
    bool down = false;
    int pending = 0;      // +1 待按下, -1 待抬起, 0 无
    int32_t screen_x = 0, screen_y = 0;
    // 双指拖拽窗口
    bool dragging = false;
    bool drag_started = false;         // 过了起手阈值才算真的拖
    int32_t grab_off_x = 0, grab_off_y = 0;
    int32_t last_cx = 0, last_cy = 0;
    View *drag_view = nullptr;
    // 单指拖边角改尺寸
    bool resizing = false;
    View *resize_view = nullptr;
    int32_t resize_x0 = 0, resize_y0 = 0;   // 手指起点
    int32_t resize_w0 = 0, resize_h0 = 0;   // 窗口起始尺寸（逻辑像素）
    int32_t resize_left = 0, resize_top = 0; // 窗口起始位置
    int resize_corner = 0;                   // 1=右下 2=左下 3=右上 4=左上
};
static InputDev g_in;

static View *hit_test(int32_t sx, int32_t sy) {
    View *best = nullptr;
    int32_t bestz = -0x7fffffff;
    for (View *v : g_srv.views) {
        if (!v->mapped || v->is_cursor || v->is_subsurface || v->bw <= 0 || v->bh <= 0) continue;
        if (sx < v->x || sy < v->y || sx >= v->x + v->ew() || sy >= v->y + v->eh()) continue;
        int32_t zz = g_base_z + v->stack_key();
        if (!best || zz > bestz) { best = v; bestz = zz; }
    }
    return best;
}

// 只有「同一个客户端」的 pointer 才能收到针对某视图的 enter/motion/button，
// 否则客户端会收到别的客户端的 surface 对象 id → "invalid object" → 整个连接 Invalid argument 断开。
static bool ptr_same_client(wl_resource *p, View *v) {
    if (!p || !v || !v->surface) return false;
    return wl_resource_get_client(p) == wl_resource_get_client(v->surface);
}

// ---------------------------------------------------------------- wl_keyboard / 按键注入
//
// 手机上没有物理键盘设备（/dev/input 里只有 gpio/pwrkey/touchpanel），
// 所以键事件靠一个 unix socket 注入：$XDG_RUNTIME_DIR/sfwm-keys.sock
//   t <文本>        —— 直接打字（裸行也当这个用）
//   k <code> <0|1>  —— 原始 evdev keycode（KEY_A=30 等）
//   s <keysym名>    —— 例如 s Return / s Escape / s Left
static struct xkb_context *g_xkb_ctx = nullptr;
static struct xkb_keymap  *g_xkb_keymap = nullptr;
static struct xkb_state   *g_xkb_state = nullptr;
static std::vector<uint32_t> g_pressed_keys;   // 当前按下的 evdev keycode

static void kb_xkb_init() {
    if (g_xkb_keymap) return;
    // Termux 的 xkb 数据不在 /usr/share/X11/xkb，环境变量没设的话补一个
    if (!getenv("XKB_CONFIG_ROOT")) {
        const char *cand[] = { "/data/data/com.termux/files/usr/share/X11/xkb", nullptr };
        for (int i = 0; cand[i]; i++) {
            if (access(cand[i], R_OK) == 0) { setenv("XKB_CONFIG_ROOT", cand[i], 0); break; }
        }
    }
    g_xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!g_xkb_ctx) { fprintf(stderr, "[kbd] xkb_context 创建失败\n"); return; }
    struct xkb_rule_names names;
    memset(&names, 0, sizeof(names));
    names.rules = "evdev";
    names.model = "pc105";
    names.layout = "us";
    g_xkb_keymap = xkb_keymap_new_from_names(g_xkb_ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!g_xkb_keymap)
        g_xkb_keymap = xkb_keymap_new_from_names(g_xkb_ctx, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!g_xkb_keymap) { fprintf(stderr, "[kbd] keymap 编译失败（XKB_CONFIG_ROOT 对不？）\n"); return; }
    g_xkb_state = xkb_state_new(g_xkb_keymap);
    printf("[kbd] xkb keymap 就绪\n");
}

// 发 keymap 给某个 wl_keyboard（wayland 要求 fd + size）。
// 用 sfwm_rt_file 而不是 memfd —— 见文件头那段说明（跨 MLS 收 fd 会被拒）。
static void kb_send_keymap(wl_resource *r) {
    kb_xkb_init();
    if (!g_xkb_keymap) return;
    char *text = xkb_keymap_get_as_string(g_xkb_keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    if (!text) return;
    size_t len = strlen(text) + 1;

    int fd = sfwm_rt_file("sfwm-keymap", 0);
    if (fd < 0) { free(text); return; }
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, text + off, len - off);
        if (n <= 0) break;
        off += n;
    }
    free(text);
    if (off != len) { close(fd); return; }
    lseek(fd, 0, SEEK_SET);
    wl_keyboard_send_keymap(r, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)len);
    close(fd);
    printf("[kbd] 已下发 keymap (%zu 字节)\n", len);
}

static void kb_send_modifiers(uint32_t serial) {
    if (!g_xkb_state) return;
    uint32_t dep = xkb_state_serialize_mods(g_xkb_state, XKB_STATE_MODS_DEPRESSED);
    uint32_t lat = xkb_state_serialize_mods(g_xkb_state, XKB_STATE_MODS_LATCHED);
    uint32_t lock = xkb_state_serialize_mods(g_xkb_state, XKB_STATE_MODS_LOCKED);
    uint32_t grp = xkb_state_serialize_layout(g_xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
    for (wl_resource *k : g_srv.keyboards)
        wl_keyboard_send_modifiers(k, serial, dep, lat, lock, grp);
}

static void kb_send_modifiers_to(wl_resource *k, uint32_t serial) {
    if (!k || !g_xkb_state) return;
    uint32_t dep = xkb_state_serialize_mods(g_xkb_state, XKB_STATE_MODS_DEPRESSED);
    uint32_t lat = xkb_state_serialize_mods(g_xkb_state, XKB_STATE_MODS_LATCHED);
    uint32_t lock = xkb_state_serialize_mods(g_xkb_state, XKB_STATE_MODS_LOCKED);
    uint32_t grp = xkb_state_serialize_layout(g_xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
    wl_keyboard_send_modifiers(k, serial, dep, lat, lock, grp);
}

// 只有「同一个客户端」的 keyboard 才能收到 enter/leave，否则 wl_resource 里的 id 对不上
static bool kb_same_client(wl_resource *k, View *v) {
    if (!k || !v || !v->surface) return false;
    return wl_resource_get_client(k) == wl_resource_get_client(v->surface);
}

static void kb_send_enter_to(wl_resource *r, View *v) {
    if (!r || !v || !v->surface) return;
    if (!kb_same_client(r, v)) return;
    uint32_t serial = wl_display_next_serial(g_srv.dpy);
    wl_array keys;
    wl_array_init(&keys);
    for (uint32_t kc : g_pressed_keys) {
        uint32_t *p = (uint32_t *)wl_array_add(&keys, sizeof(uint32_t));
        if (p) *p = kc;
    }
    printf("[kbd] enter -> kb#%u(client %p) surface#%u(client %p)\n",
           wl_resource_get_id(r), (void *)wl_resource_get_client(r),
           v->surface ? wl_resource_get_id(v->surface) : 0,
           v->surface ? (void *)wl_resource_get_client(v->surface) : nullptr);
    wl_keyboard_send_enter(r, serial, v->surface, &keys);
    wl_array_release(&keys);
    kb_send_modifiers_to(r, serial);
}

// ---------------------------------------------------------------- zwp_text_input_v3
// 客户端（GTK/Qt/foot…）靠它声明「我要输入法」。合成器这边负责：
//   enter/leave（跟随键盘焦点）、把注入器的文本用 commit_string 送进去、
//   每次客户端 commit 后回一个 done（协议要求）。
// 这是「中文能打字」的关键 —— 键盘注入走 US keymap 是没有 CJK keycode 的。
struct TiState {
    bool enabled = false;
    View *focus = nullptr;
    uint32_t hint = 0, purpose = 0;
    uint32_t serial = 0;      // 协议要求：done 的 serial = commit 计数（这里收发共用一个递增计数器）
};

static void ti_destroyed(wl_resource *r) {
    for (size_t i = 0; i < g_srv.text_inputs.size(); i++)
        if (g_srv.text_inputs[i] == r) { g_srv.text_inputs.erase(g_srv.text_inputs.begin() + i); break; }
}
static TiState *ti_state(wl_resource *r) { return (TiState *)wl_resource_get_user_data(r); }

static void ti_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void ti_enable(wl_client *, wl_resource *r) {
    if (TiState *st = ti_state(r)) {
        st->enabled = true;
        // 客户端「启用输入法」= 有输入框拿到焦点 —— 这就是要弹键盘的信号。
        // 以前这步没人管，所以只能手动喊 ime on。
        if (!g_ime_on) ime_send(true);
    }
}
static void ti_disable(wl_client *, wl_resource *r) {
    if (TiState *st = ti_state(r)) {
        st->enabled = false;
        if (g_ime_on) ime_send(false);
    }
}
static void ti_set_surrounding(wl_client *, wl_resource *, const char *, int32_t, int32_t) {}
static void ti_set_cause(wl_client *, wl_resource *, uint32_t) {}
static void ti_set_content_type(wl_client *, wl_resource *r, uint32_t hint, uint32_t purpose) {
    if (TiState *st = ti_state(r)) { st->hint = hint; st->purpose = purpose; }
}
static void ti_set_cursor_rect(wl_client *, wl_resource *, int32_t, int32_t, int32_t, int32_t) {
    // P2：内置软键盘要靠它避开光标
}
static void ti_commit(wl_client *, wl_resource *r) {
    // 客户端每次 commit 之后，合成器必须回 done（serial 用递增计数）
    TiState *st = ti_state(r);
    uint32_t serial = st ? ++st->serial : 1;
    zwp_text_input_v3_send_done(r, serial);
}
static void ti_set_available_actions(wl_client *, wl_resource *, struct wl_array *) {}
static void ti_show_panel(wl_client *, wl_resource *r) {
    // 这就是「用户点了一下输入框、要弹键盘」的标准信号。以前只打印了一行日志，
    // 所以点输入框没任何反应（enable 只在客户端第一次启用输入法时发一次）。
    printf("[ti] show_input_panel -> 拉起键盘\n");
    if (!g_ime_on) ime_send(true);
}
static void ti_hide_panel(wl_client *, wl_resource *r) {
    printf("[ti] hide_input_panel -> 收起键盘\n");
    if (g_ime_on) ime_send(false);
}
static const struct zwp_text_input_v3_interface ti_impl = {
    .destroy = ti_destroy,
    .enable = ti_enable,
    .disable = ti_disable,
    .set_surrounding_text = ti_set_surrounding,
    .set_text_change_cause = ti_set_cause,
    .set_content_type = ti_set_content_type,
    .set_cursor_rectangle = ti_set_cursor_rect,
    .commit = ti_commit,
    .set_available_actions = ti_set_available_actions,
    .show_input_panel = ti_show_panel,
    .hide_input_panel = ti_hide_panel,
};

static void ti_update_focus(View *oldv, View *newv) {
    if (g_srv.text_inputs.empty()) return;
    for (wl_resource *r : g_srv.text_inputs) {
        TiState *st = ti_state(r);
        if (!st) continue;
        wl_client *c = wl_resource_get_client(r);
        if (oldv && oldv->surface && wl_resource_get_client(oldv->surface) == c) {
            zwp_text_input_v3_send_leave(r, oldv->surface);
            st->focus = nullptr;
        }
        if (newv && newv->surface && wl_resource_get_client(newv->surface) == c) {
            zwp_text_input_v3_send_enter(r, newv->surface);
            st->focus = newv;
        }
    }
}

// 有 enabled 的 text-input 就用 commit_string（支持任意 UTF-8，含中文）
static bool ti_commit_text(const char *utf8) {
    View *v = g_srv.kb_focus;
    bool sent = false;
    for (wl_resource *r : g_srv.text_inputs) {
        TiState *st = ti_state(r);
        if (!st || !st->enabled) continue;
        if (v && v->surface && wl_resource_get_client(v->surface) != wl_resource_get_client(r)) continue;
        zwp_text_input_v3_send_commit_string(r, utf8);
        zwp_text_input_v3_send_done(r, ++st->serial);
        sent = true;
    }
    return sent;
}

// 拼音/组词阶段（还没上屏）走 preedit_string，客户端会画成带下划线的预编辑串。
// 传空串 = 清掉预编辑（finishComposingText）。
static bool ti_preedit_text(const char *utf8) {    View *v = g_srv.kb_focus;
    bool sent = false;
    uint32_t len = 0;
    if (utf8 && *utf8) len = (uint32_t)strlen(utf8);
    for (wl_resource *r : g_srv.text_inputs) {
        TiState *st = ti_state(r);
        if (!st || !st->enabled) continue;
        if (v && v->surface && wl_resource_get_client(v->surface) != wl_resource_get_client(r)) continue;
        zwp_text_input_v3_send_preedit_string(r, (utf8 && *utf8) ? utf8 : "", (int32_t)len, (int32_t)len);
        zwp_text_input_v3_send_done(r, ++st->serial);
        sent = true;
    }
    return sent;
}

static void ti_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void ti_mgr_get(wl_client *c, wl_resource *mgr, uint32_t id, wl_resource *seat) {
    (void)seat;
    uint32_t ver = wl_resource_get_version(mgr);
    wl_resource *r = wl_resource_create(c, &zwp_text_input_v3_interface, ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    TiState *st = new TiState();
    wl_resource_set_implementation(r, &ti_impl, st, [](wl_resource *res) {
        ti_destroyed(res);
        delete (TiState *)wl_resource_get_user_data(res);
    });
    g_srv.text_inputs.push_back(r);
    printf("[ti] 新 text-input (v%u)\n", ver);
    if (g_srv.kb_focus) ti_update_focus(nullptr, g_srv.kb_focus);
}
static const struct zwp_text_input_manager_v3_interface ti_mgr_impl = {
    .destroy = ti_mgr_destroy,
    .get_text_input = ti_mgr_get,
};
static void bind_ti_mgr(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 2) version = 2;
    wl_resource *r = wl_resource_create(c, &zwp_text_input_manager_v3_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &ti_mgr_impl, nullptr, nullptr);
}

/**
 * 强制让所有客户端「退出输入法焦点」。
 *
 * 这是「安卓那边把键盘收掉了」时该做的事：用户一个返回手势把键盘划走，
 * 桌面这边 GTK 的输入框却还挂着 IME 焦点（光标还在闪、还在等输入法），
 * 两边状态就脱节了。发 disable + leave，客户端才会真正 focus_out 掉输入法上下文。
 */
static void ti_force_disable() {
    int n = 0;
    for (wl_resource *r : g_srv.text_inputs) {
        TiState *st = ti_state(r);
        if (!st) continue;
        // text-input-v3 里 disable 是「客户端→服务端」的 request，服务端没有对应事件。
        // 服务端能让客户端 focus_out 的手段只有一个：发 leave(null) —— surface 为空即「离开当前输入目标」。
        if (st->enabled || st->focus) {
            zwp_text_input_v3_send_leave(r, nullptr);
            st->focus = nullptr;
            st->enabled = false;
            zwp_text_input_v3_send_done(r, ++st->serial);
            n++;
        }
    }
    if (n) printf("[ti] 键盘被系统收起 -> 已让 %d 个客户端退出输入法焦点\n", n);
}

// ---------------------------------------------------------------- 输入法状态通道
// 为什么不走 socket：模块是个 untrusted_app，连不进我们的 unix socket，TCP 5599 也不可靠。
// 它写自己数据目录里的一个文件（只 app 自己能写），我们用 root 把它读出来。
static const char *IME_STATE_FILE = "/data/data/org.sfwm.ime/files/ime-state";
static wl_event_source *g_ime_timer = nullptr;

static void ime_apply(int32_t h, bool on) {
    if (!on) h = 0;
    if (h == g_ime_h && on == g_ime_on) return;
    g_ime_h = h;
    g_ime_on = on;
    if (!on) ti_force_disable();
    // 注意：不能只 place() —— 裁剪写在 blit() 里，而客户端画完一帧就不再送帧，
    // 高度变化时不会有新帧来触发它。所以这里必须主动重发一次 setGeometry。
    for (View *v : g_srv.views) {
        if (!v->mapped || v->is_x_root) continue;
        v->place();
        if (v->layer.win) {
            v->layer.setGeometry(sf::Rect{0, 0, v->bw, v->bh},
                                 sf::Rect{0, 0, v->bw, v->bh});
        }
    }
    printf("[ime] 控件状态: 高=%d 键盘=%s\n", g_ime_h, on ? "显示" : "隐藏");
    // 键盘收起时必须把它的图层也藏掉 —— 否则它会以“最后一帧”（通常是一整块不透明背景）
    // 一直盖在窗口底部，而且还会吃掉点击（表现为“不该遮挡的时候遮挡”）。
    for (View *v : g_srv.views)
        if (v->is_ime && v->layer.win) {
            v->layer.setVisible(on);
            printf("[ime] 输入法图层 -> %s\n", on ? "显示" : "隐藏");
        }
}

static int on_ime_tick(void *) {
    // 键盘上沿（来自 dumpsys，一次性文件）：高度 = 屏幕高 - 上沿
    // 注意：不再读 app 写的那份状态文件 —— 它只能是 208/0（insets 相对窗口算，不可信），
    // 会和这里算出的 1024 互相覆盖，造成画面抽插。
    if (g_ime_on) {
        const char *inset = "/data/local/tmp/sfwm-ime-inset";
        FILE *g = fopen(inset, "r");
        if (g) {
            int top = -1;
            if (fscanf(g, "%d", &top) == 1 && top > 0 && top < g_out_h) {
                int32_t h = g_out_h - top;
                if (h > 0 && h != g_ime_h) {
                    printf("[ime] 从窗口几何取到高度 = %d（上沿 %d）\n", h, top);
                    ime_apply(h, true);
                }
            }
            fclose(g);
            unlink(inset);      // 一次性，避免收起后又把它读回来
        }
    }
    if (g_ime_timer) wl_event_source_timer_update(g_ime_timer, 300);
    return 0;
}

// 最上层的普通 toplevel（没有指针焦点时兜底用）
static View *front_view() {
    View *best = nullptr;
    int32_t bestz = -0x7fffffff;
    for (View *v : g_srv.views) {
        if (!v->mapped || v->is_cursor || v->is_subsurface || !v->surface) continue;
        int32_t zz = v->stack_key();
        if (!best || zz >= bestz) { best = v; bestz = zz; }
    }
    return best;
}

static void kb_set_focus(View *v) {
    if (v == g_srv.kb_focus) return;
    View *old_focus = g_srv.kb_focus;
    uint32_t serial = wl_display_next_serial(g_srv.dpy);
    if (g_srv.kb_focus && g_srv.kb_focus->surface) {
        for (wl_resource *k : g_srv.keyboards)
            if (kb_same_client(k, g_srv.kb_focus))
                wl_keyboard_send_leave(k, serial, g_srv.kb_focus->surface);
    }
    g_srv.kb_focus = v;
    printf("[kbd] 焦点 -> %s\n", v ? (v->title[0] ? v->title : "view") : "(无)");
    ti_update_focus(old_focus, v);   // text-input 的 enter/leave 跟着键盘焦点走
    // 换焦点时把按下的键全放掉，避免卡键
    for (uint32_t kc : g_pressed_keys) {
        for (wl_resource *k : g_srv.keyboards)
            if (kb_same_client(k, v))
                wl_keyboard_send_key(k, serial, now_ms(), kc, WL_KEYBOARD_KEY_STATE_RELEASED);
    }
    if (g_xkb_state) {
        for (uint32_t kc : g_pressed_keys) xkb_state_update_key(g_xkb_state, kc + 8, XKB_KEY_UP);
    }
    g_pressed_keys.clear();
    if (v) {
        for (wl_resource *k : g_srv.keyboards) kb_send_enter_to(k, v);
    }
}

static void kb_key(uint32_t code, bool pressed) {
    if (g_srv.keyboards.empty()) { printf("[kbd] 没有客户端在听键盘\n"); return; }
    if (!g_srv.kb_focus) kb_set_focus(front_view());
    if (!g_srv.kb_focus) { printf("[kbd] 没有可聚焦窗口，丢弃按键 %u\n", code); return; }
    kb_xkb_init();
    if (g_xkb_state)
        xkb_state_update_key(g_xkb_state, code + 8, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
    if (pressed) {
        bool have = false;
        for (uint32_t kc : g_pressed_keys) if (kc == code) have = true;
        if (!have) g_pressed_keys.push_back(code);
    } else {
        for (size_t i = 0; i < g_pressed_keys.size(); i++)
            if (g_pressed_keys[i] == code) { g_pressed_keys.erase(g_pressed_keys.begin() + i); break; }
    }
    uint32_t serial = wl_display_next_serial(g_srv.dpy);
    uint32_t t = now_ms();
    for (wl_resource *k : g_srv.keyboards) {
        if (!kb_same_client(k, g_srv.kb_focus)) continue;
        wl_keyboard_send_key(k, serial, t, code,
                             pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
        kb_send_modifiers_to(k, serial);
    }
    static int key_logs = 0;
    if (key_logs++ < 12)
        printf("[kbd] -> code=%u %s (keyboards=%zu focus=%s)\n", code,
               pressed ? "按下" : "抬起", g_srv.keyboards.size(),
               g_srv.kb_focus ? (g_srv.kb_focus->title[0] ? g_srv.kb_focus->title : "view") : "无");
}

// 找一个能产生该 keysym 的 keycode（先试 level0，再试 level1=Shift）
static bool kb_find_keycode(xkb_keysym_t sym, uint32_t *code, bool *need_shift) {
    if (!g_xkb_keymap) return false;
    for (xkb_keycode_t kc = 9; kc < 256; kc++) {
        const xkb_keysym_t *syms = nullptr;
        int n = xkb_keymap_key_get_syms_by_level(g_xkb_keymap, kc, 0, 0, &syms);
        if (n == 1 && syms[0] == sym) { *code = kc - 8; *need_shift = false; return true; }
        n = xkb_keymap_key_get_syms_by_level(g_xkb_keymap, kc, 0, 1, &syms);
        if (n == 1 && syms[0] == sym) { *code = kc - 8; *need_shift = true; return true; }
    }
    return false;
}

static void kb_tap(uint32_t code, bool shift) {
    // 注意：KEY_LEFTSHIFT 是 linux/input.h 里的宏（=42），不能拿来当变量名
    if (shift) kb_key(KEY_LEFTSHIFT, true);
    kb_key(code, true);
    kb_key(code, false);
    if (shift) kb_key(KEY_LEFTSHIFT, false);
}

static void pointer_motion(int32_t sx, int32_t sy, bool send);
static void mouse_button(int32_t x, int32_t y, int pressed);
static void mouse_click(int32_t x, int32_t y);

static void kb_type_text(const char *s) {
    kb_xkb_init();
    // 有 text-input 的客户端优先走 commit_string —— 这条能送任意 UTF-8（含中文），
    // 而 keymap 注入要找 keycode，CJK 根本找不到。
    if (ti_commit_text(s)) { printf("[ime] commit_string: %s\n", s); return; }
    if (!g_xkb_keymap) return;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t cp = 0;
        // 解 UTF-8
        if (*p < 0x80) { cp = *p++; }
        else if ((*p & 0xE0) == 0xC0 && p[1]) { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
        else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
            cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3;
        } else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
            cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) |
                 ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); p += 4;
        } else { p++; continue; }

        if (cp == '\n') { kb_tap(KEY_ENTER, false); continue; }
        if (cp == '\t') { kb_tap(KEY_TAB, false); continue; }

        uint32_t code = 0;
        bool shift = false;
        if (kb_find_keycode((xkb_keysym_t)cp, &code, &shift)) {
            kb_tap(code, shift);
        } else {
            printf("[kbd] 这个字符敲不出来: U+%04X\n", cp);
        }
    }
}

static void kb_send_symbol(const char *name) {
    kb_xkb_init();
    if (!g_xkb_keymap) return;
    xkb_keysym_t sym = xkb_keysym_from_name(name, XKB_KEYSYM_NO_FLAGS);
    if (sym == XKB_KEY_NoSymbol) { printf("[kbd] 不认识的 keysym: %s\n", name); return; }
    uint32_t code = 0;
    bool shift = false;
    if (kb_find_keycode(sym, &code, &shift)) kb_tap(code, shift);
    else printf("[kbd] keysym %s 没有对应 keycode\n", name);
}

// 拉起/收起安卓软键盘：命令写 settings，由 LSPosed 模块里的悬浮窗服务 SfwmImeService 执行。
// 为什么不由 sfwm 自己弹：输入法要求一个 Java View（ViewRootImpl + window token + InputConnection），
// SurfaceComposer 图层没有这一层，IME 永远指不到它。
static void ime_kick_geometry();
static void ime_send(bool show) {
    g_ime_on = show;
    // 首选：往下发给已登记的长连接（相当于 Wayland 服务端发事件）
    if (g_ime_fd >= 0) {
        const char *msg = show ? "activate\n" : "deactivate\n";
        if (write(g_ime_fd, msg, strlen(msg)) >= 0) {
            printf("[ime] %s（下发给 IME 客户端 fd=%d）\n", show ? "activate" : "deactivate", g_ime_fd);
            if (show) ime_kick_geometry();
            return;
        }
        printf("[ime] 下发失败，客户端连接已断，回退到 settings 通道\n");
        g_ime_fd = -1;
    }
    // 回退：没有客户端连接时，照旧用 settings + am
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long ms = (long)ts.tv_sec * 1000 + (long)(ts.tv_nsec / 1000000);
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "/system/bin/am start-foreground-service -n org.sfwm.ime/.SfwmImeService -f 0x20 >/dev/null 2>&1; "
             "settings put global sfwm_ime %d; "
             "settings put global sfwm_ime_seq %ld",
             show ? 1 : 2, ms);
    int rc = system(cmd);
    printf("[ime] %s rc=%d seq=%ld（回退通道）\n", show ? "拉起" : "收起", rc, ms);
    if (show) ime_kick_geometry();
}

// 键盘弹出后去捡一次窗口几何（IME 窗口的 mGivenContentInsets）算真实高度。
// 这是目前唯一可靠的来源：insets 恒为 208，IMM 推荐高度恒为 0。
static void ime_kick_geometry() {
    system("( sleep 1; dumpsys window windows 2>/dev/null "
           "| sed -n '/InputMethod}/,/Window #/p' "
           "| grep -o 'mGivenContentInsets=\\[0,[0-9]*\\]' | head -1 | tr -dc '0-9' "
           "> /data/local/tmp/sfwm-ime-inset ) &");
}

static void kb_command(const std::string &line, int fd) {
    if (line.empty()) return;
    printf("[kbd] 命令: %s\n", line.c_str());
    const char *s = line.c_str();
    if (line == "ime-register") {
        // IME 宿主登记为「常驻客户端」：这条连接保持住，以后由 sfwm 往它下发 activate/deactivate
        g_ime_fd = fd;
        printf("[ime] IME 客户端已登记 fd=%d\n", fd);
        return;
    }
    if (line.compare(0, 5, "imeh ") == 0) {
        int h = atoi(s + 5);
        if (h < 0) h = 0;
        g_ime_h = h;
        for (View *v : g_srv.views) if (v->mapped) v->place();
        printf("[ime] 客户端上报高度 = %d\n", g_ime_h);
        return;
    }
    if (line.compare(0, 2, "t ") == 0) { kb_type_text(s + 2); }
    else if (line.compare(0, 5, "text ") == 0) { kb_type_text(s + 5); }
    else if (line.compare(0, 2, "k ") == 0) {
        unsigned code = 0, state = 1;
        if (sscanf(s + 2, "%u %u", &code, &state) >= 1) kb_key(code, state != 0);
    } else if (line.compare(0, 2, "s ") == 0) { kb_send_symbol(s + 2); }
    else if (line.compare(0, 2, "m ") == 0) {
        int x = 0, y = 0;
        if (sscanf(s + 2, "%d %d", &x, &y) == 2) { pointer_motion(x, y, true); }
    } else if (line.compare(0, 2, "c ") == 0) {
        int x = 0, y = 0, st = 1;
        int n = sscanf(s + 2, "%d %d %d", &x, &y, &st);
        // c x y      = 完整点击（按下+抬起）
        // c x y 0/1  = 只抬起/只按下
        if (n >= 3) mouse_button(x, y, st != 0);
        else if (n == 2) mouse_click(x, y);
    } else if (line.compare(0, 2, "p ") == 0) {
        int x = 0, y = 0;
        if (sscanf(s + 2, "%d %d", &x, &y) == 2) mouse_click(x, y);
    } else if (line.compare(0, 4, "pre ") == 0) {
        // 预编辑串（拼音未上屏），空内容用 "pre" 单独一行来清
        ti_preedit_text(s + 4);
    } else if (line == "pre") {
        ti_preedit_text("");
    } else if (line == "snap") {
        g_snap_on = !g_snap_on;
        printf("[wm] 吸附 = %s（阈值 %d dp）\n", g_snap_on ? "开" : "关", g_snap_band_dp);
    } else if (line.compare(0, 5, "snap ") == 0) {
        std::string arg = line.substr(5);
        if (arg == "on" || arg == "1" || arg == "true") g_snap_on = true;
        else if (arg == "off" || arg == "0" || arg == "false") g_snap_on = false;
        else { int n = atoi(arg.c_str()); if (n >= 0 && n <= 200) g_snap_band_dp = n; }
        printf("[wm] 吸附 = %s（阈值 %d dp）\n", g_snap_on ? "开" : "关", g_snap_band_dp);
    } else if (line == "dump") {
        printf("[dump] screen=%dx%d scale=%d gesture=%d owner=%d\n",
               g_out_w, g_out_h, g_scale, (int)g_in_gesture, touch_owner_value());
        for (View *v : g_srv.views) {
            if (!v) continue;
            printf("[dump] %-28s %p x=%-5d y=%-5d buf=%dx%d scale=%d z=%-5d mapped=%d cur=%d sub=%d layer=%d title='%s'\n",
                   v->app_id[0] ? v->app_id : "?", (void *)v, v->x, v->y, v->bw, v->bh,
                   v->buf_scale, g_base_z + v->stack_key(), (int)v->mapped,
                   (int)v->is_cursor, (int)v->is_subsurface, (int)v->is_layer,
                   v->title);
        }
        printf("[dump] pointer=%d,%d focus=%p\n", g_srv.pointer_x, g_srv.pointer_y,
               (void *)g_srv.pointer_focus);
    } else if (line == "fit") {
        // 一键救回：把最大的普通窗口夹进屏幕并居中（对话框按钮通常就在右下角）
        View *v = nullptr;
        for (View *o : g_srv.views) {
            if (!o->mapped || o->is_cursor || o->is_layer || o->is_subsurface) continue;
            if (!v || (int64_t)o->bw * o->bh > (int64_t)v->bw * v->bh) v = o;
        }
        if (!v) { printf("[wm] fit: 没有窗口\n"); return; }
        const int32_t ox = v->x, oy = v->y;
        if (v->bw <= g_out_w) v->x = (g_out_w - v->bw) / 2;
        if (v->bh <= g_out_h) v->y = (g_out_h - v->bh) / 2;
        v->clamp_on_screen();
        printf("[wm] fit '%s' (%d,%d)->(%d,%d) buf=%dx%d 屏幕=%dx%d\n",
               v->title[0] ? v->title : "view", ox, oy, v->x, v->y, v->bw, v->bh, g_out_w, g_out_h);
    } else if (line == "max") {
        // 让最上层的普通窗口铺满输出：关键是**发 configure 告诉客户端尺寸**，
        // 光改 v->x/y 没用（buffer 还是客户端自己定的 1280x720）。
        // 按“面积最大”选，而不是 z 最高 —— 否则会选中 64x64 的光标 surface。
        View *v = nullptr;
        for (View *o : g_srv.views) {
            if (!o->mapped || o->is_cursor || o->is_layer || o->is_subsurface) continue;
            if (!v || (int64_t)o->bw * o->bh > (int64_t)v->bw * v->bh) v = o;
        }
        if (!v) { printf("[wm] max: 没有可最大化的窗口\n"); return; }
        // ★ 必须用客户端的 buf_scale，不能用 g_scale。
        //   g_scale 是 sfwm 自己的 UI 缩放（--scale=3），和客户端无关；
        //   用它去除会把 1272x2800 的屏幕变成 424x933，窗口缩成一小块。
        const int32_t sc = client_scale(v);
        v->x = 0;
        v->y = 0;
        v->req_w = g_out_w / sc;
        v->req_h = g_out_h / sc;
        // 只发尺寸不够：很多客户端（含 nested compositor）只在状态里有 MAXIMIZED 时才采纳。
        v->want_maximized = true;
        toplevel_send_configure(v);
        v->place();
        printf("[wm] max '%s' -> 请求 %dx%d 逻辑（屏幕 %dx%d，scale=%d）\n",
               v->title[0] ? v->title : "view", v->req_w, v->req_h, g_out_w, g_out_h, sc);
    } else if (line.rfind("zoom ", 0) == 0) {
        // 调试：手动模拟「捏合中」的合成器侧缩放（不碰客户端）
        View *v = nullptr;
        for (View *o : g_srv.views) {
            if (!o->mapped || o->is_cursor || o->is_layer || o->is_subsurface) continue;
            if (!v || (int64_t)o->bw * o->bh > (int64_t)v->bw * v->bh) v = o;
        }
        if (!v) { printf("[wm] zoom: 没有窗口\n"); return; }
        float z = (float)atof(line.c_str() + 5);
        if (z < 0.1f) z = 0.1f;
        if (z > 4.0f) z = 4.0f;
        const int32_t cx = v->x + v->bw / 2, cy = v->y + v->bh / 2;
        v->x = cx - (int32_t)(v->bw * z) / 2;
        v->y = cy - (int32_t)(v->bh * z) / 2;
        v->set_zoom(z);
        printf("[wm] zoom %.3f -> '%s' dst=%dx%d @ (%d,%d)，buffer 保持 %dx%d（没发 configure）\n",
               z, v->title[0] ? v->title : "view",
               (int)(v->bw * z), (int)(v->bh * z), v->x, v->y, v->bw, v->bh);
    } else if (line == "zoomend") {
        // 调试：模拟「松手」——发一次 configure 落实尺寸
        View *v = nullptr;
        for (View *o : g_srv.views) {
            if (!o->mapped || o->is_cursor || o->is_layer || o->is_subsurface) continue;
            if (!v || (int64_t)o->bw * o->bh > (int64_t)v->bw * v->bh) v = o;
        }
        if (!v) { printf("[wm] zoomend: 没有窗口\n"); return; }
        const int32_t sc0 = client_scale(v);
        v->req_w = (int32_t)(v->bw * v->zoom) / sc0;
        v->req_h = (int32_t)(v->bh * v->zoom) / sc0;
        if (v->req_w < 120) v->req_w = 120;
        if (v->req_h < 120) v->req_h = 120;
        toplevel_send_configure(v);
        printf("[wm] zoomend zoom=%.3f -> 请求 %dx%d 逻辑（buffer %dx%d）\n",
               v->zoom, v->req_w, v->req_h, v->bw, v->bh);
    } else if (line == "ct") {
        // 点击穿透开关：开了之后我们的图层不再抢触摸，安卓界面完全可点
        g_clickthrough = !g_clickthrough;
        if (g_clickthrough) { g_in.resizing = false; g_in.resize_view = nullptr; g_in.dragging = false; g_in.drag_view = nullptr; g_in.down = false; g_in.pending = 0; }
        for (View *v : g_srv.views) if (v->mapped) v->place();
        printf("[wm] 点击穿透 = %s\n", g_clickthrough ? "开" : "关");
    } else if (line.compare(0, 3, "ct ") == 0) {
        // 注意 "on"/"off" 首字母都是 'o'，必须整词比较
        std::string arg = line.substr(3);
        bool val = g_clickthrough;
        if (arg == "on" || arg == "1" || arg == "true" || arg == "yes") val = true;
        else if (arg == "off" || arg == "0" || arg == "false" || arg == "no") val = false;
        else val = !g_clickthrough;
        g_clickthrough = val;
        if (g_clickthrough) { g_in.resizing = false; g_in.resize_view = nullptr; g_in.dragging = false; g_in.drag_view = nullptr; g_in.down = false; g_in.pending = 0; }
        for (View *v : g_srv.views) if (v->mapped) v->place();
        printf("[wm] 点击穿透 = %s\n", g_clickthrough ? "开" : "关");
    } else if (line == "ime") {
        ime_send(!g_ime_on);
    } else if (line.compare(0, 4, "ime ") == 0) {
        std::string a = line.substr(4);
        if (a == "on" || a == "1" || a == "true") ime_send(true);
        else if (a == "off" || a == "0" || a == "false") ime_send(false);
        else ime_send(!g_ime_on);
    } else if (line.compare(0, 5, "imeh ") == 0) {
        int h = atoi(s + 5);
        if (h < 0) h = 0;
        g_ime_h = h;
        for (View *v : g_srv.views) if (v->mapped) v->place();
        printf("[ime] 高度 = %d\n", g_ime_h);
    } else if (line.compare(0, 9, "imestate ") == 0) {
        int st = atoi(s + 9);
        g_ime_on = st != 0;
        if (!g_ime_on) {
            g_ime_h = 0;
            ti_force_disable();     // 键盘被系统收掉 -> 让客户端同步退出输入法焦点
        }
        for (View *v : g_srv.views) if (v->mapped) v->place();
        printf("[ime] 状态 = %s (h=%d)\n", g_ime_on ? "显示" : "隐藏", g_ime_h);
    } else if (line == "hide") {
        // 把合成器输出藏起来（让出屏幕给安卓界面），但进程继续跑、继续收注入
        for (View *v : g_srv.views) if (v->layer.win) v->layer.hide();
        printf("[sfwm] 图层已隐藏\n");
    } else if (line == "show") {
        for (View *v : g_srv.views) if (v->mapped) v->place();
        printf("[sfwm] 图层已恢复\n");
    }
    else if (line == "help") {
        printf("[kbd] 用法: t <文本> | k <evdev码> <0|1> | s <keysym名> | m <x> <y> | c <x> <y> <0|1> | p <x> <y>\n");
    } else {
        kb_type_text(s);   // 裸行 = 直接打字
    }
}

// ---- 注入 socket
static void key_conn_close(KeyConn *kc) {
    if (!kc) return;
    if (kc->fd == g_ime_fd) {
        g_ime_fd = -1;
        printf("[ime] IME 客户端连接断开\n");
    }
    if (kc->src) wl_event_source_remove(kc->src);
    for (size_t i = 0; i < g_srv.key_conns.size(); i++) {
        if (g_srv.key_conns[i] == kc) { g_srv.key_conns.erase(g_srv.key_conns.begin() + i); break; }
    }
    close(kc->fd);
    delete kc;
}

static int on_key_conn(int fd, uint32_t mask, void *data) {
    KeyConn *kc = (KeyConn *)data;
    // 先读数据：客户端往往「发完就 close」，此时 mask 里 READABLE 和 HANGUP 同时置位，
    // 必须先取走数据，否则整条命令就丢了。
    bool hangup = (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) != 0;
    if (mask & WL_EVENT_READABLE) {
        char tmp[1024];
        ssize_t n = read(fd, tmp, sizeof(tmp) - 1);
        if (n <= 0) { key_conn_close(kc); return 0; }
        tmp[n] = 0;
        kc->buf.append(tmp, (size_t)n);
        size_t pos;
        while ((pos = kc->buf.find('\n')) != std::string::npos) {
            std::string line = kc->buf.substr(0, pos);
            kc->buf.erase(0, pos + 1);
            if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
            kb_command(line, fd);
        }
        if (!hangup) return 0;
    }
    // 对端已关闭：把残留的最后一行（没带换行的）也处理掉再收工
    if (!kc->buf.empty()) { std::string line = kc->buf; kc->buf.clear(); kb_command(line, kc->fd); }
    key_conn_close(kc);
    return 0;
}

static int on_key_accept(int fd, uint32_t, void *) {
    int c = accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
    if (c < 0) return 0;
    printf("[kbd] 注入连接已接受 fd=%d\n", c);
    KeyConn *kc = new KeyConn();
    kc->fd = c;
    g_srv.key_conns.push_back(kc);
    kc->src = wl_event_loop_add_fd(g_srv.loop, c, WL_EVENT_READABLE, on_key_conn, kc);
    return 0;
}

// 在指定路径开一个注入 socket 并挂上 accept 处理器。
// 注意 chmod 0666 只解决「同 uid 之外的进程能不能连」——DAC 还要能穿过父目录。
// Termux 的 $HOME/.run 是 0700 u0_a391，别的 app（SwiftKey 等）根本进不去；
// 所以另外在 /data/local/tmp 开一个全 uid 可达的。
static int key_listen_on(const std::string &path, bool is_primary) {
    unlink(path.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) { fprintf(stderr, "[kbd] socket 创建失败(%s): %s\n", path.c_str(), strerror(errno)); return -1; }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 8) < 0) {
        fprintf(stderr, "[kbd] bind/listen 失败(%s): %s\n", path.c_str(), strerror(errno));
        close(fd);
        return -1;
    }
    chmod(path.c_str(), 0666);
    if (is_primary) { g_srv.key_listen_fd = fd; g_srv.key_sock_path = path; }
    wl_event_loop_add_fd(g_srv.loop, fd, WL_EVENT_READABLE, on_key_accept, nullptr);
    printf("[kbd] 注入 socket: %s\n", path.c_str());
    return fd;
}

// 抽象命名空间 unix socket（@name）。没有文件系统节点，不受目录 DAC 限制，
// 也不受 /data/local/tmp 的 SELinux 类型牵连 —— 普通 app 最容易连的就是它。
static int key_listen_abstract(const char *name) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) { fprintf(stderr, "[kbd] 抽象 socket 创建失败: %s\n", strerror(errno)); return -1; }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t n = strlen(name);
    if (n + 1 > sizeof(addr.sun_path)) { close(fd); return -1; }
    addr.sun_path[0] = '\0';
    memcpy(addr.sun_path + 1, name, n);
    socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);
    if (bind(fd, (struct sockaddr *)&addr, len) < 0 || listen(fd, 8) < 0) {
        fprintf(stderr, "[kbd] 抽象 bind 失败(@%s): %s\n", name, strerror(errno));
        close(fd);
        return -1;
    }
    wl_event_loop_add_fd(g_srv.loop, fd, WL_EVENT_READABLE, on_key_accept, nullptr);
    printf("[kbd] 注入 socket: @%s\n", name);
    return fd;
}

// 127.0.0.1 上的 TCP 端口 —— 保底方案：有 INTERNET 权限的 app 连 loopback 一定被允许
static int key_listen_tcp(int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) { fprintf(stderr, "[kbd] tcp socket 创建失败: %s\n", strerror(errno)); return -1; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 8) < 0) {
        fprintf(stderr, "[kbd] tcp bind 失败(:%d): %s\n", port, strerror(errno));
        close(fd);
        return -1;
    }
    wl_event_loop_add_fd(g_srv.loop, fd, WL_EVENT_READABLE, on_key_accept, nullptr);
    printf("[kbd] 注入 socket: 127.0.0.1:%d\n", port);
    return fd;
}

// 把 socket 开在目标 app 自己的数据目录里：DAC 同 uid、SELinux 同 app_data_file label，
// 是唯一「普通 app 一定连得上」的文件系统位置。（/data/local/tmp 会被 SELinux 拦）
static int key_listen_on_appdir(const char *pkg) {
    std::string dir = std::string("/data/data/") + pkg;
    struct stat st;
    if (stat(dir.c_str(), &st) < 0) { printf("[kbd] %s 不存在，跳过 appdir socket\n", dir.c_str()); return -1; }
    std::string files = dir + "/files";
    mkdir(files.c_str(), 0770);
    std::string path = files + "/sfwm-keys.sock";
    int fd = key_listen_on(path, false);
    if (fd < 0) return -1;
    chown(path.c_str(), st.st_uid, st.st_gid);
    chmod(path.c_str(), 0600);
    printf("[kbd] appdir socket uid=%u gid=%u\n", (unsigned)st.st_uid, (unsigned)st.st_gid);
    return fd;
}

// 看门狗：攥住的帧超过 150ms 还没被下一帧顶掉，就立刻读掉。
//   两个作用：
//     1) 客户端如果只提交一次（单缓冲/静止画面），我们不会把它一直攥着不放；
//     2) 万一哪次 wl_buffer 生命周期没兜住，也不会无限期留着一个悬空指针。
static wl_event_source *g_defer_timer = nullptr;
static int on_defer_tick(void *) {
    const uint32_t nowm = now_ms();
    for (View *v : g_srv.views) {
        if (!v->read_next || nowm - v->read_next_ms < 150) continue;
        if (v->hold) { wl_list_remove(&v->hold->lis.link); delete v->hold; v->hold = nullptr; }
        wl_resource *b = v->read_next;
        v->read_next = nullptr;
        v->blit(b);
        printf("[wm] 看门狗：把攥了 %ums 的帧读掉\n", nowm - v->read_next_ms);
    }
    if (g_defer_timer) wl_event_source_timer_update(g_defer_timer, 150);
    return 0;
}

static void sfwm_keys_init() {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    key_listen_on(std::string(rt ? rt : "/data/local/tmp") + "/sfwm-keys.sock", true);
    // 跨 app 通道：/data/local/tmp 本身是 0771 shell:shell，others 有 x（可穿过），
    // 文件 0666 就能连。但 SELinux 会拦 untrusted_app → shell_data_file，实测 Permission denied。
    if (!rt || std::string(rt) != "/data/local/tmp") key_listen_on("/data/local/tmp/sfwm-keys.sock", false);
    // 真正好用的那条：把 socket 开在输入法自己的数据目录里（同 uid、同 app_data_file label）
    key_listen_on_appdir("com.touchtype.swiftkey");
    // IME 宿主（我们自己那个 APK）也算一个客户端：它连进来后保持长连接，
    // sfwm 往下发 activate/deactivate，它往上发 height。
    key_listen_on_appdir("org.sfwm.ime");
    key_listen_abstract("sfwm-keys");
    key_listen_tcp(5599);
    kb_xkb_init();
    if (g_srv.loop) {
        g_defer_timer = wl_event_loop_add_timer(g_srv.loop, on_defer_tick, nullptr);
        if (g_defer_timer) wl_event_source_timer_update(g_defer_timer, 150);
    }
}

// ---- wl_keyboard 资源
static void kb_destroyed(wl_resource *r) {
    for (size_t i = 0; i < g_srv.keyboards.size(); i++)
        if (g_srv.keyboards[i] == r) { g_srv.keyboards.erase(g_srv.keyboards.begin() + i); break; }
}
static void kb_release(wl_client *, wl_resource *r) {
    kb_destroyed(r);
    wl_resource_destroy(r);
}
static const struct wl_keyboard_interface keyboard_impl = {
    .release = kb_release,
};

static void seat_get_keyboard(wl_client *c, wl_resource *seat, uint32_t id) {
    uint32_t ver = wl_resource_get_version(seat);
    wl_resource *r = wl_resource_create(c, &wl_keyboard_interface, ver, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &keyboard_impl, nullptr, kb_destroyed);
    g_srv.keyboards.push_back(r);
    kb_send_keymap(r);
    if (ver >= 4) wl_keyboard_send_repeat_info(r, 25, 600);
    // 已经有焦点就补一个 enter，否则客户端永远收不到按键
    View *v = g_srv.kb_focus ? g_srv.kb_focus : front_view();
    if (v) {
        if (!g_srv.kb_focus) g_srv.kb_focus = v;
        kb_send_enter_to(r, v);
    }
    printf("[kbd] 新客户端拿到键盘 (v%u)\n", ver);
}

static void pointer_motion(int32_t sx, int32_t sy, bool send) {
    g_srv.pointer_x = sx;
    g_srv.pointer_y = sy;
    g_cur_x = sx;
    g_cur_y = sy;
    // 光标层跟着触点走：客户端自己给了 cursor surface 就画它的，否则画内置的
    if (g_srv.cursor_view && g_srv.cursor_view->layer.win) {
        g_srv.cursor_view->x = sx - g_srv.cursor_hot_x;
        g_srv.cursor_view->y = sy - g_srv.cursor_hot_y;
        g_srv.cursor_view->place();
        cursor_hide();
    } else {
        // 只在手指按下期间画光标。
        // 原因：SurfaceFlinger 不允许非系统 uid 的图层改 touchOcclusionMode
        // （实测 setTrustedOverlay/setDropInputMode 调用成功，但 dumpsys input
        // 依旧是 touchOcclusionMode=BLOCK_UNTRUSTED）→ 光标那块 72x72 像素
        // 会吞掉落在它上面的下一次点击。把“空闲时不可见”就能彻底避开。
        if (g_in.down) cursor_present();
    }
    if (g_srv.pointers.empty()) return;
    View *v = hit_test(sx, sy);
    // 隐式指针 grab：按住期间锁定按下时的窗口。
    // 否则手指滑出窗口就会发 leave，客户端把 button 一丢，后续点击全失效。
    if (g_in.down && g_btn_view && g_btn_view->mapped) v = g_btn_view;
    uint32_t serial = wl_display_next_serial(g_srv.dpy);
    uint32_t t = now_ms();

    if (v != g_srv.pointer_focus) {
        printf("[input] 焦点切换: %s -> %s\n",
               g_srv.pointer_focus ? (g_srv.pointer_focus->title[0] ? g_srv.pointer_focus->title : "view") : "(无)",
               v ? (v->title[0] ? v->title : "view") : "(无)");
        if (g_srv.pointer_focus && g_srv.pointer_focus->surface) {
            for (wl_resource *p : g_srv.pointers)
                if (ptr_same_client(p, g_srv.pointer_focus))
                    wl_pointer_send_leave(p, serial, g_srv.pointer_focus->surface);
        }
        g_srv.pointer_focus = v;
        if (v && v->surface) {
            for (wl_resource *p : g_srv.pointers) {
                if (!ptr_same_client(p, v)) continue;
                // wl_fixed_t 是 24.8 定点数；且 surface 局部坐标是逻辑像素（= 物理 / buffer_scale）
                wl_pointer_send_enter(p, serial, v->surface,
                                      wl_fixed_from_int(v->to_local_x(sx)),
                                      wl_fixed_from_int(v->to_local_y(sy)));
                if (wl_resource_get_version(p) >= 5) wl_pointer_send_frame(p);
            }
        }
    } else if (v && send) {
        for (wl_resource *p : g_srv.pointers) {
            if (!ptr_same_client(p, v)) continue;
            wl_pointer_send_motion(p, t,
                                   wl_fixed_from_int(v->to_local_x(sx)),
                                   wl_fixed_from_int(v->to_local_y(sy)));
            if (wl_resource_get_version(p) >= 5) wl_pointer_send_frame(p);
        }
    }
}

// 合成鼠标：给注入器用（手机上不想伸手点时，也能驱动任何客户端）
static void mouse_button(int32_t x, int32_t y, int pressed) {
    pointer_motion(x, y, true);
    View *v = g_srv.pointer_focus;
    if (!v) { printf("[mouse] (%d,%d) 处没有窗口\n", x, y); return; }
    if (pressed) kb_set_focus(v);
    uint32_t serial = wl_display_next_serial(g_srv.dpy);
    // Qt 这类客户端要先拿到一次 motion（确认光标位置）才认后续的 button
    for (wl_resource *p : g_srv.pointers) {
        if (!ptr_same_client(p, v)) continue;
        wl_pointer_send_motion(p, now_ms(),
                               wl_fixed_from_int(v->to_local_x(x)),
                               wl_fixed_from_int(v->to_local_y(y)));
        if (wl_resource_get_version(p) >= 5) wl_pointer_send_frame(p);
    }
    for (wl_resource *p : g_srv.pointers) {
        if (!ptr_same_client(p, v)) continue;
        wl_pointer_send_button(p, serial, now_ms(), BTN_LEFT, pressed ? 1 : 0);
        if (wl_resource_get_version(p) >= 5) wl_pointer_send_frame(p);
    }
    int nmatched = 0;
    for (wl_resource *p : g_srv.pointers) if (ptr_same_client(p, v)) nmatched++;
    printf("[mouse] %s (%d,%d) -> %s local=(%d,%d) pointers=%zu matched=%d\n", pressed ? "按下" : "抬起", x, y,
           v->title[0] ? v->title : "view", v->to_local_x(x), v->to_local_y(y),
           g_srv.pointers.size(), nmatched);
}

static void mouse_click(int32_t x, int32_t y) {
    mouse_button(x, y, 1);
    mouse_button(x, y, 0);
}

// 抢占：把点中的窗口提到所有窗口之上。
// 用“当前最大 z + 1”而不是 ++next_z，避免反复点击后 z 无限膨胀。
static void raise_view(View *v) {
    // 弹窗（xdg_popup）也有 parent，但没有父亲图层可依附，必须允许它自己提升
    if (!v || v->is_cursor || (v->parent && !v->is_popup)) return;
    // 整屏窗口是「底图」，不参与点击提升，否则点一下就把桌面盖成黑板
    if (v->bw * 100 >= g_out_w * 95 && v->bh * 100 >= g_out_h * 95) {
        printf("[wm] '%s' 是底图，不参与提升\n",
               v->title[0] ? v->title : (v->app_id[0] ? v->app_id : "view"));
        return;
    }
    int32_t top = -1;
    for (View *o : g_srv.views)
        if (o->mapped && !o->is_cursor && !o->parent && o->z > top) top = o->z;
    if (v->z >= top) return;
    v->z = top + 1;
    v->place();
    printf("[wm] 抢占 -> '%s' 到最前 z=%d\n",
           v->title[0] ? v->title : (v->app_id[0] ? v->app_id : "view"), v->z);
}

// 手指是否落在“标题栏”条上（顶部 title_h 像素）：这里是合成器管辖，可以拖窗口
static int32_t title_h(View *v) {
    const int32_t s = v && v->buf_scale > 0 ? v->buf_scale : 1;
    int32_t h = 44 * s;
    // 至少要盖满角带（32*g_scale），否则顶部那一小条会落进“只能缩上边”的区里，
    // 表现就是“想拖窗口，结果一拖就变高/变矮”。
    const int32_t band = 32 * (g_scale > 0 ? g_scale : 1);
    if (h < band) h = band;
    if (v && h > v->bh / 3) h = v->bh / 3;
    return h;
}
static bool in_title(View *v, int32_t sx, int32_t sy) {
    if (!v) return false;
    return sy >= v->y && sy < v->y + title_h(v);
}

// 手指是否落在窗口的缩放区：
//   1=右下 2=左下 3=右上 4=左上 5=右边 6=左边 7=下边，0=不是
// 以前只认四个角，所以「拖左/右边框改宽度」完全没反应。
// 上边（原 8）故意不做：手机上顶部是用来拖窗口的，再做缩放会互相抢，
// 结果就是“想移窗口却变成放大”（这个 bug 已经踩过）。
static int corner_hit(View *v, int32_t sx, int32_t sy) {
    if (!v || v->bw <= 0 || v->bh <= 0) return 0;
    int32_t m = 32 * (g_scale > 0 ? g_scale : 1);
    if (m > v->bw / 3) m = v->bw / 3;
    if (m > v->bh / 3) m = v->bh / 3;
    if (m < 12) m = 12;
    // 顶部整条留给“移动”，不参与缩放
    if (sy <= v->y + m) return 0;
    bool nearR = sx >= v->x + v->bw - m, nearL = sx <= v->x + m;
    bool nearB = sy >= v->y + v->bh - m;
    if (nearR && nearB) return 1;
    if (nearL && nearB) return 2;
    if (nearR) return 5;
    if (nearL) return 6;
    if (nearB) return 7;
    return 0;
}

// 这个 view 背后的客户端是不是「正开着输入法」（有输入框拿到焦点）
static bool ti_enabled_for(View *v) {
    if (!v || !v->surface) return false;
    wl_client *c = wl_resource_get_client(v->surface);
    for (wl_resource *r : g_srv.text_inputs) {
        TiState *st = ti_state(r);
        if (st && st->enabled && wl_resource_get_client(r) == c) return true;
    }
    return false;
}

static void input_button(bool pressed) {
    View *v = g_srv.pointer_focus;
    uint32_t serial = wl_display_next_serial(g_srv.dpy);
    uint32_t t = now_ms();
    if (pressed) {
        pointer_motion(g_in.screen_x, g_in.screen_y, true);   // 先确认焦点/位置
        v = g_srv.pointer_focus;
        if (v) {
            g_btn_view = v;           // 记住按下的窗口，UP 时必须还给它
            kb_set_focus(v);          // 点哪个窗口，键盘就给哪个
            raise_view(v);            // 抢占：点谁谁到最前
        }
        // 点到「已经启用输入法」的客户端就弹键盘（兜底：某些客户端不会再发 show_input_panel）
        if (!g_ime_on && ti_enabled_for(v)) ime_send(true);
    } else if (!v && g_btn_view) {
        // 抬起时 focus 已经丢了（手指滑出窗口）：仍然要把 release 补发出去
        v = g_btn_view;
    }
    if (!v) { g_btn_view = nullptr; return; }

    // wl_touch
    for (wl_resource *tr : g_srv.touches) {
        if (!ptr_same_client(tr, v)) continue;
        if (pressed)
            wl_touch_send_down(tr, serial, t, v->surface, 0,
                               wl_fixed_from_int((g_in.screen_x - v->x) / v->buf_scale),
                               wl_fixed_from_int((g_in.screen_y - v->y) / v->buf_scale));
        else
            wl_touch_send_up(tr, serial, t, 0);
        if (wl_resource_get_version(tr) >= 5) wl_touch_send_frame(tr);
    }
    // wl_pointer 按钮
    for (wl_resource *p : g_srv.pointers) {
        if (!ptr_same_client(p, v)) continue;
        wl_pointer_send_button(p, serial, t, BTN_LEFT, pressed ? 1 : 0);
        if (wl_resource_get_version(p) >= 5) wl_pointer_send_frame(p);
    }
    if (!pressed) g_btn_view = nullptr;
    printf("[input] %s screen=(%d,%d) surface=(%d,%d) scale=%d target=%s z=%d rect=(%d,%d %dx%d)\n",
           pressed ? "DOWN" : "UP",
           g_in.screen_x, g_in.screen_y,
           v->to_local_x(g_in.screen_x),
           v->to_local_y(g_in.screen_y),
           v->buf_scale,
           v->title[0] ? v->title : "view", v->z, v->x, v->y, v->bw, v->bh);
}

static void input_touch_motion() {
    View *v = g_srv.pointer_focus;
    if (!v) return;
    uint32_t t = now_ms();
    for (wl_resource *tr : g_srv.touches) {
        wl_touch_send_motion(tr, t, 0,
                            wl_fixed_from_int((g_in.screen_x - v->x) / v->buf_scale),
                            wl_fixed_from_int((g_in.screen_y - v->y) / v->buf_scale));
        if (wl_resource_get_version(tr) >= 5) wl_touch_send_frame(tr);
    }
}

static void raw_to_screen(int32_t rx, int32_t ry, int32_t *sx, int32_t *sy) {
    int32_t w = g_in.xmax - g_in.xmin, h = g_in.ymax - g_in.ymin;
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    int32_t x = (int32_t)((int64_t)(rx - g_in.xmin) * g_out_w / w);
    int32_t y = (int32_t)((int64_t)(ry - g_in.ymin) * g_out_h / h);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= g_out_w) x = g_out_w - 1;
    if (y >= g_out_h) y = g_out_h - 1;
    *sx = x; *sy = y;
}

// 拖边角/双指缩放时不要每帧都发 configure：
// 客户端每次都要重新算布局、重画、换 buffer —— 120Hz 发等于一直在闪。
// 规则：尺寸至少变 6 逻辑像素，或者距上次至少 60ms。
static uint32_t g_cfg_ms = 0;
static int32_t g_cfg_w = -1, g_cfg_h = -1;
// 捏合过程中客户端每次收到 configure 都要重建 swapchain → 发太勤就是整屏闪。
// 所以这里两道闸门：时间闸门（捏合中 250ms，其余 120ms）+ 变化闸门（<24 逻辑像素）。
// 老代码是「dw<6 && dh<6 && <60ms 才 return」，等于尺寸一变就每帧都发。
static uint32_t g_cfg_min_interval = 120;

static void maybe_configure(View *v) {
    const uint32_t now = now_ms();
    const int32_t  dw  = abs(v->req_w - g_cfg_w), dh = abs(v->req_h - g_cfg_h);
    if (g_cfg_w >= 0 && now - g_cfg_ms < g_cfg_min_interval) return;
    if (g_cfg_w >= 0 && dw < 24 && dh < 24 && now - g_cfg_ms < 400) return;
    g_cfg_ms = now;
    g_cfg_w  = v->req_w;
    g_cfg_h  = v->req_h;
    toplevel_send_configure(v);
}

// 松手吸附：贴近屏幕左右边/中线/顶部时对齐，避免每次都要手调到像素级
static void snap_view(View *v) {
    if (!v) return;
    const int32_t M = 40 * (g_scale > 0 ? g_scale : 1);
    bool moved = false;
    int32_t w = v->bw;
    if (v->x > -M && v->x < M) { v->x = 0; moved = true; }
    else if (v->x + w > g_out_w - M && v->x + w < g_out_w + M) { v->x = g_out_w - w; moved = true; }
    else {
        int32_t c = v->x + w / 2, m = g_out_w / 2;
        if (c > m - M && c < m + M) { v->x = m - w / 2; moved = true; }
    }
    if (v->y > -M && v->y < M) { v->y = 0; moved = true; }
    if (moved) { v->place(); printf("[wm] 吸附 -> (%d,%d)\n", v->x, v->y); }
}

// ---------------------------------------------------------------- 触摸状态机
// 参考 Hyprland 的做法：owner 一旦确定，到抬起为止不再改变。
//   OWNER_CLIENT : 全部事件转给客户端（含多指 pinch）
//   OWNER_MOVE   : 长按 400ms 转过来的窗口移动（锚定 + 阈值 + 过程吸附）
//   OWNER_RESIZE : 起手落在角带里，改尺寸（对角固定）
//   OWNER_NONE   : 空闲
enum TouchOwner { OWNER_NONE = 0, OWNER_CLIENT, OWNER_MOVE, OWNER_RESIZE, OWNER_PINCH };

static TouchOwner g_owner = OWNER_NONE;
static int touch_owner_value(void) { return (int)g_owner; }
static View *g_gesture_view = nullptr;      // MOVE / RESIZE 的作用对象
static View *g_client_view = nullptr;       // CLIENT 模式的目标（次指 up 要用）
static int32_t g_press_x = 0, g_press_y = 0;
static bool g_move_started = false;
// 长按已布防、但还没真正拖动：抬起时仍然要把这次按压当成一次点击
static bool g_lp_pending_click = false;
static wl_event_source *g_lp_timer = nullptr;
static int32_t g_sec_id[10] = {0};           // 次指 → wl_touch id
static int32_t g_next_touch_id = 1;
static int32_t g_primary_slot = -1;
static int32_t g_prev_primary = -1;
static int32_t g_pinch_d0 = 0;               // 双指起始距离
static int32_t g_pinch_w0 = 0, g_pinch_h0 = 0;   // 起始逻辑尺寸
// 双指中点 + 起始窗口原点：用来做“以两指中点为中心缩放”
static int32_t g_pinch_mx = 0, g_pinch_my = 0;
static int32_t g_pinch_px = 0, g_pinch_py = 0;
// 手势开始时就把渲染倍率“冻结”：blit 里会自校准 buf_scale，
// 如果每帧都去读活值，锚定算式算出的原点会在两个值之间来回翻 —— 就是“瞬移”。
static int32_t g_pinch_sc = 1;
static float   g_pinch_zoom0 = 1.f;   // 手势开始时的 zoom，连续捏合不会跳

static void touch_ev(View *v, int32_t id, int32_t sx, int32_t sy, int kind) {
    if (!v || !v->surface) return;
    uint32_t serial = wl_display_next_serial(g_srv.dpy);
    uint32_t t = now_ms();
    for (wl_resource *tr : g_srv.touches) {
        if (!ptr_same_client(tr, v)) continue;
        if (kind == 0)
            wl_touch_send_down(tr, serial, t, v->surface, id,
                               wl_fixed_from_int(v->to_local_x(sx)),
                               wl_fixed_from_int(v->to_local_y(sy)));
        else if (kind == 1)
            wl_touch_send_motion(tr, t, id,
                                 wl_fixed_from_int(v->to_local_x(sx)),
                                 wl_fixed_from_int(v->to_local_y(sy)));
        else
            wl_touch_send_up(tr, serial, t, id);
        if (wl_resource_get_version(tr) >= 5) wl_touch_send_frame(tr);
    }
}

// 次指同步：第 2 根以后的每根手指各拿一个独立的 wl_touch id，
// 这样客户端的 pinch / 双指滚动才真的能用（以前合成器自己吃掉了）
static void sec_sync(View *v) {
    if (!v) return;
    // 主指换了（原来那根抬起来、另一根升格为主指）→ 旧的主指得先把它的次指 id 抬掉
    if (g_primary_slot >= 0 && g_primary_slot < 10 && g_sec_id[g_primary_slot]) {
        touch_ev(v, g_sec_id[g_primary_slot], 0, 0, 2);
        g_sec_id[g_primary_slot] = 0;
    }
    for (int i = 0; i < 10; i++) {
        if (i == g_primary_slot) continue;
        int32_t sx, sy;
        if (g_in.slots[i].id >= 0) {
            raw_to_screen(g_in.slots[i].x, g_in.slots[i].y, &sx, &sy);
            if (!g_sec_id[i]) {
                g_sec_id[i] = g_next_touch_id++;
                touch_ev(v, g_sec_id[i], sx, sy, 0);
            } else {
                touch_ev(v, g_sec_id[i], sx, sy, 1);
            }
        } else if (g_sec_id[i]) {
            touch_ev(v, g_sec_id[i], 0, 0, 2);
            g_sec_id[i] = 0;
        }
    }
}

static void sec_release_all() {
    View *v = g_client_view;
    for (int i = 0; i < 10; i++) {
        if (g_sec_id[i]) {
            if (v) touch_ev(v, g_sec_id[i], 0, 0, 2);
            g_sec_id[i] = 0;
        }
    }
}

// 长按 400ms：把已经发给客户端的那根手指“收回来”，转成窗口移动
static int on_longpress(void *) {
    if (g_lp_timer) wl_event_source_timer_update(g_lp_timer, 0);
    if (g_clickthrough || g_owner != OWNER_CLIENT) return 0;
    View *v = g_client_view ? g_client_view : hit_test(g_press_x, g_press_y);
    if (!v || v->is_subsurface || v->is_cursor) return 0;

    // ★ 这里**不能**给客户端发 up：那会把用户正在点的按钮/滑块直接吞掉。
    //   改成：按压保持住，进入“预备移动”。抬手没移动 -> 照常算点击；
    //   真的拖动超过阈值 -> 那时才发 up 取消点击（见 OWNER_MOVE 分支）。
    g_lp_pending_click = true;

    g_owner = OWNER_MOVE;
    g_gesture_view = v;
    g_move_started = false;
    g_in.grab_off_x = v->x - g_in.screen_x;
    g_in.grab_off_y = v->y - g_in.screen_y;
    g_in.last_cx = g_in.screen_x;
    g_in.last_cy = g_in.screen_y;
    raise_view(v);
    printf("[wm] 长按 -> 预备移动 '%s'（未拖动则仍算点击）\n", v->title[0] ? v->title : "view");
    return 0;
}

// 过程吸附：只改“将要应用的位置”，不改窗口本身
// 必须有滞回（hysteresis）：否则手指停在吸附边界上，窗口会在两个位置之间反复跳 —— 就是闪烁。
static void snap_pos(View *v, int32_t &px, int32_t &py) {
    if (!g_snap_on || g_snap_band_dp <= 0) return;
    // 窗口比屏幕还大时吸附毫无意义，只会把它拉开屏幕（“乱飞”的帮凶）
    if (!v || v->bw > g_out_w || v->bh > g_out_h) return;
    const int32_t BAND = g_snap_band_dp * (g_scale > 0 ? g_scale : 1);   // 进入阈值
    const int32_t REL  = BAND * 2;                            // 退出阈值（更大 = 滞回）
    const int32_t w    = v->bw;
    const int32_t m    = g_out_w / 2;

    if (g_snap_bits & 1) { if (px > REL) g_snap_bits &= ~1; else px = 0; }
    else if (g_snap_bits & 4) { if (px + w < g_out_w - REL) g_snap_bits &= ~4; else px = g_out_w - w; }
    else if (g_snap_bits & 2) { if (abs(px + w / 2 - m) > REL) g_snap_bits &= ~2; else px = m - w / 2; }
    else if (abs(px) < BAND) { g_snap_bits |= 1; px = 0; }
    else if (abs(px + w - g_out_w) < BAND) { g_snap_bits |= 4; px = g_out_w - w; }
    else if (abs(px + w / 2 - m) < BAND) { g_snap_bits |= 2; px = m - w / 2; }

    if (g_snap_bits & 8) { if (py > REL) g_snap_bits &= ~8; else py = 0; }
    else if (abs(py) < BAND) { g_snap_bits |= 8; py = 0; }
}

static void handle_frame() {
    // 点击穿透模式下 sfwm 完全不碰触摸：不然 Android 和合成器会同时收到同一根手指
    if (g_clickthrough) return;

    // 键盘弹着：落在键盘区域的手指全给安卓输入法（候选词条、按键都属于它），
    // 但只要已经有拖动/缩放在进行中，就让它做完，免得手势半途被冻住。
    if (g_ime_h > 0 && (g_owner == OWNER_NONE || g_owner == OWNER_CLIENT)) {
        for (int i = 0; i < 10; i++) {
            if (g_in.slots[i].id < 0) continue;
            int32_t bsx, bsy;
            raw_to_screen(g_in.slots[i].x, g_in.slots[i].y, &bsx, &bsy);
            if (bsy >= g_out_h - g_ime_h) return;
        }
    }
    g_in_gesture = (g_owner == OWNER_MOVE || g_owner == OWNER_RESIZE || g_owner == OWNER_PINCH);
    int nf = 0;
    int64_t sumx = 0, sumy = 0;
    int32_t fx = 0, fy = 0;
    g_primary_slot = -1;
    for (int i = 0; i < 10; i++) {
        if (g_in.slots[i].id < 0) continue;
        int32_t sx, sy;
        raw_to_screen(g_in.slots[i].x, g_in.slots[i].y, &sx, &sy);
        if (nf == 0) { fx = sx; fy = sy; g_primary_slot = i; }
        sumx += sx; sumy += sy;
        nf++;
    }

    // ---------- 全部抬起 ----------
    if (nf == 0) {
        if (g_lp_timer) wl_event_source_timer_update(g_lp_timer, 0);
        if (g_owner == OWNER_RESIZE) {
            View *v = g_gesture_view;
            printf("[wm] 拖边角结束 -> '%s' 逻辑尺寸 %dx%d\n",
                   v ? (v->title[0] ? v->title : "view") : "无",
                   v ? (v->bw / (v->buf_scale > 0 ? v->buf_scale : 1)) : 0,
                   v ? (v->bh / (v->buf_scale > 0 ? v->buf_scale : 1)) : 0);
        } else if (g_owner == OWNER_MOVE) {
            View *v = g_gesture_view;
            if (v && g_move_started)
                printf("[input] 移动结束 -> '%s' @ (%d,%d)\n",
                       v->title[0] ? v->title : "view", v->x, v->y);
        } else if (g_owner == OWNER_CLIENT) {
            sec_release_all();
        } else if (g_owner == OWNER_PINCH) {
            View *v = g_gesture_view;
            printf("[wm] 缩放结束 -> '%s' 逻辑尺寸 %dx%d\n",
                   v ? (v->title[0] ? v->title : "view") : "无",
                   v ? (v->bw / (v->buf_scale > 0 ? v->buf_scale : 1)) : 0,
                   v ? (v->bh / (v->buf_scale > 0 ? v->buf_scale : 1)) : 0);
        }
        // ── 手势结束才“收编”：操控过程中完全不干预（不重建图层、不夹位置、不发 configure）
        // 抬起就把光标藏起来：避免它空闲时压住下一次点击（见 pointer_motion 里的说明）
        cursor_hide();
        // ── 捏合结束：把合成器侧的 zoom 落实成客户端的新尺寸 ──────────────
        //   手势中 buffer 一直没变（放大是糊的）。这里只发**一次** configure，
        //   客户端按新尺寸重绘后糊就没了。zoom 先保留，等新 buffer 到达再归 1，
        //   否则会先“弹回”原尺寸再变大，很难看。
        if (g_gesture_view && g_owner == OWNER_PINCH && g_gesture_view->zoom != 1.f) {
            View *gv = g_gesture_view;
            const int32_t sc0 = client_scale(gv);
            // 目标尺寸夹到屏幕以内：configure_bounds 报的就是屏幕，
            // 超出的请求客户端直接无视（白跑一趟，还会重建 swapchain = 闪）。
            const int32_t maxw = g_out_w / sc0, maxh = g_out_h / sc0;
            int32_t fw = (int32_t)(gv->bw * gv->zoom) / sc0;
            int32_t fh = (int32_t)(gv->bh * gv->zoom) / sc0;
            if (fw > maxw) fw = maxw;
            if (fh > maxh) fh = maxh;
            if (fw < 120) fw = 120;
            if (fh < 120) fh = 120;
            const int32_t curw = gv->bw / (sc0 > 0 ? sc0 : 1);
            const int32_t curh = gv->bh / (sc0 > 0 ? sc0 : 1);
            const int32_t ddx = fw > curw ? fw - curw : curw - fw;
            const int32_t ddy = fh > curh ? fh - curh : curh - fh;
            if (ddx <= 4 && ddy <= 4) {
                // 已经是屏幕尺寸：这次 configure 什么也变不了，只会让客户端
                // 重建一次 swapchain（视觉上就是“松手时闪一下”）。
                // 保留 zoom 当放大镜，等下次捏小才会重新变清晰。
                printf("[wm] 缩放结束：已到屏幕上限，保持视觉缩放 %.3f（模糊属正常）\n", gv->zoom);
            } else {
                gv->req_w = fw;
                gv->req_h = fh;
                toplevel_send_configure(gv);
                printf("[wm] 缩放结束：buffer %dx%d zoom=%.3f -> 请求 %dx%d 逻辑\n",
                       gv->bw, gv->bh, gv->zoom, gv->req_w, gv->req_h);
            }
        }
        if (g_gesture_view && g_owner == OWNER_RESIZE) {
            View *gv = g_gesture_view;
            if (gv->bw > g_out_w || gv->bh > g_out_h) {
                // 用「实际 buffer ÷ 上次请求的逻辑尺寸」反推真实倍率
                //（Qt 经常不按 set_buffer_scale 来，直接用 buf_scale 会算错一圈）
                int32_t eff = gv->buf_scale > 0 ? gv->buf_scale : 1;
                if (gv->req_w > 0 && gv->bw > 0) {
                    int32_t e2 = (gv->bw + gv->req_w / 2) / gv->req_w;
                    if (e2 >= 1 && e2 <= 8) eff = e2;
                }
                gv->req_w = (g_out_w - 60) / eff;
                gv->req_h = (g_out_h - 60) / eff;
                if (gv->req_w < 200) gv->req_w = 200;
                if (gv->req_h < 200) gv->req_h = 200;
                toplevel_send_configure(gv);
                printf("[wm] 手势结束收编：%dx%d 超出屏幕 -> 请求 %dx%d 逻辑（实测倍率 %d）\n",
                       gv->bw, gv->bh, gv->req_w, gv->req_h, eff);
            }
        }
        // ★ 手势结束后必须把窗口拉回屏幕：否则捏合/拖拽能把它推到屏幕外，
        //   图层还在渲染（SurfaceFlinger 不裁），但 hit_test 永远命中不到 → "点不动"。
        if (g_gesture_view && g_gesture_view->mapped) {
            View *v = g_gesture_view;
            const int32_t ox = v->x, oy = v->y;
            v->clamp_on_screen();
            if (ox != v->x || oy != v->y)
                printf("[wm] 手势结束拉回屏幕: (%d,%d)->(%d,%d) buf=%dx%d\n",
                       ox, oy, v->x, v->y, v->bw, v->bh);
        }

        // 捏合/拖角结束时补发一次最终尺寸（过程中被限流跳过的那些帧最后都要落地）
        g_in_gesture = false;
        if (g_cfg_min_interval != 120) g_cfg_min_interval = 120;
        if (g_gesture_view && g_gesture_view->mapped && g_gesture_view->req_w > 0)
            toplevel_send_configure(g_gesture_view);

        if (g_in.down) { g_in.down = false; input_button(false); }
        g_lp_pending_click = false;
        g_owner = OWNER_NONE;
        g_gesture_view = nullptr;
        g_client_view = nullptr;
        g_move_started = false;
        g_snap_bits = 0;
        g_in_drag = false;
        g_in.pending = 0;
        g_arm_corner = 0;
        return;
    }

    // ---------- 客户端想让某个窗口跟着走（xdg_toplevel.move）----------
    if (g_srv.move_view && g_owner != OWNER_MOVE && g_owner != OWNER_RESIZE) {
        View *v = g_srv.move_view;
        g_srv.move_view = nullptr;
        if (g_owner == OWNER_CLIENT && g_in.down) { g_in.down = false; input_button(false); }
        sec_release_all();
        g_owner = OWNER_MOVE;
        g_gesture_view = v;
        g_move_started = false;
        g_snap_bits = 0;
        g_in_drag = true;
        g_in.grab_off_x = v->x - fx;
        g_in.grab_off_y = v->y - fy;
        g_in.last_cx = fx; g_in.last_cy = fy;
        raise_view(v);
        printf("[wm] 客户端请求移动 '%s'\n", v->title[0] ? v->title : "view");
    }

    g_in.screen_x = fx;
    g_in.screen_y = fy;

    // ---------- owner: PINCH（双指改窗口大小）----------
    if (g_owner == OWNER_PINCH) {
        View *v = g_gesture_view;
        g_in.pending = 0;
        if (!v || !v->mapped) { g_owner = OWNER_NONE; g_gesture_view = nullptr; return; }
        if (nf < 2) return;                       // 松到只剩一根：等全抬起才结束
        int32_t ax = 0, ay = 0, bx = 0, by = 0, got = 0;
        for (int i = 0; i < 10 && got < 2; i++) {
            if (g_in.slots[i].id < 0) continue;
            int32_t sx, sy;
            raw_to_screen(g_in.slots[i].x, g_in.slots[i].y, &sx, &sy);
            if (got == 0) { ax = sx; ay = sy; got = 1; } else { bx = sx; by = sy; got = 2; }
        }
        if (got < 2) return;
        int32_t ddx = bx - ax, ddy = by - ay;
        int32_t d = (int32_t)sqrt((double)(ddx * ddx + ddy * ddy));
        double scale = g_pinch_d0 > 0 ? (double)d / (double)g_pinch_d0 : 1.0;
        if (scale < 0.25) scale = 0.25;
        if (scale > 4.0) scale = 4.0;
        // ★ 手势期间**完全不动客户端**：只记 zoom，图层 dst 尺寸跟着变。
        //   没有 configure → 客户端不重建 swapchain → 不闪、不抖、不打架。
        //   代价是放大时糊（原 buffer 分辨率不够），抬手后发一次 configure 就清晰了。
        float nz = g_pinch_zoom0 * (float)scale;
        if (nz < 0.1f) nz = 0.1f;
        // 允许放大到超过屏幕：那是“放大镜”效果（content 会糊，但至少看得见、点得准）。
        //   真正的上限交给手势结束时处理 —— 那里只在尺寸确实会变时才发 configure。
        if (nz > 4.0f) nz = 4.0f;
        // 以两指中点为中心：手指下面那块内容始终留在手指下面
        v->x = g_pinch_mx - (int32_t)((g_pinch_mx - g_pinch_px) * scale);
        v->y = g_pinch_my - (int32_t)((g_pinch_my - g_pinch_py) * scale);
        v->set_zoom(nz);      // 父层 + 所有子表面一起缩放
        {
            static int dbgn = 0;
            if ((dbgn++ & 15) == 0)
                printf("[wm] 双指缩放 zoom=%.3f (指距 %d->%d)\n", scale, g_pinch_d0, d);
        }
        return;
    }

    // ---------- owner: RESIZE ----------
    if (g_owner == OWNER_RESIZE) {
        View *v = g_gesture_view;
        if (v && v->mapped) {
            const int32_t sc = v->buf_scale > 0 ? v->buf_scale : 1;
            int32_t dx = fx - g_in.resize_x0, dy = fy - g_in.resize_y0;
            int32_t w = g_in.resize_w0, h = g_in.resize_h0;
            int32_t nx = g_in.resize_left, ny = g_in.resize_top;
            const int c = g_in.resize_corner;
            // 逐轴判断：纯左边/右边只改宽，纯上边/下边只改高（以前用 if/else 判，
            // 边缘带根本不会被识别，所以只能拖四个角）
            const bool eR = (c == 1 || c == 3 || c == 5);
            const bool eL = (c == 2 || c == 4 || c == 6);
            const bool eB = (c == 1 || c == 2 || c == 7);
            const bool eT = (c == 3 || c == 4 || c == 8);
            if (eR) w += dx;
            else if (eL) { w -= dx; nx += dx; }
            if (eB) h += dy;
            else if (eT) { h -= dy; ny += dy; }
            const int32_t minp = 120 * sc;
            if (w < minp) { if (eL) nx = g_in.resize_left + g_in.resize_w0 - minp; w = minp; }
            if (h < minp) { if (eT) ny = g_in.resize_top + g_in.resize_h0 - minp; h = minp; }
            // 尺寸不设上限：桌面隐喻里窗口本来就允许比屏幕大，用户自己能缩回去。
            // 之前这里和 blit 里的“夹回屏幕”一起把位置硬改了，反而显得“乱飞”。
            if (nx != v->x || ny != v->y) { v->x = nx; v->y = ny; v->place(); }
            int32_t lw = w / sc, lh = h / sc;
            if (lw != v->req_w || lh != v->req_h) {
                v->req_w = lw;
                v->req_h = lh;
                maybe_configure(v);
                printf("[wm] 拖边角 -> 请求 %dx%d 逻辑像素\n", lw, lh);
            }
        }
        g_in.pending = 0;
        return;
    }

    // ---------- owner: MOVE ----------
    if (g_owner == OWNER_MOVE) {
        g_reason = "move";
        View *v = g_gesture_view;
        if (v) {
            // ★ 阈值 24dp（以前是 8dp）。触屏手指抖动就有 1–2mm（scale=2 时 8dp 只有 16 物理像素），
            //   于是每次“轻点小按钮”都会越过阈值 → 进入 MOVE → 窗口跟着手指跑
            //   → 按钮从手指底下溜走（“点不了”），同时每帧重新 place 造成闪烁。
            const int32_t TH = 24 * (g_scale > 0 ? g_scale : 1);
            if (!g_move_started) {
                int32_t ddx = fx - g_in.last_cx, ddy = fy - g_in.last_cy;
                if (ddx * ddx + ddy * ddy < TH * TH) { g_in.pending = 0; return; }
                g_move_started = true;
                // 这一刻才算真的拖窗口：撤销那根手指的按压（点击作废），
                // 并用“当前手指位置”重算锚点 —— 用长按时刻的旧锚点会让窗口瞬移。
                if (g_lp_pending_click) {
                    g_lp_pending_click = false;
                    if (g_in.down) { g_in.down = false; input_button(false); }
                    printf("[wm] 长按后拖动 -> 取消点击，开始移动窗口\n");
                }
                g_in.grab_off_x = v->x - fx;
                g_in.grab_off_y = v->y - fy;
            }
            int32_t nx = fx + g_in.grab_off_x, ny = fy + g_in.grab_off_y;
            const int32_t keep = 60 * (g_scale > 0 ? g_scale : 1);
            if (nx > g_out_w - keep) nx = g_out_w - keep;
            if (ny > g_out_h - keep) ny = g_out_h - keep;
            if (nx < -(v->bw - keep)) nx = -(v->bw - keep);
            if (ny < 0) ny = 0;
            snap_pos(v, nx, ny);      // 过程吸附：每帧都对齐
            if (nx != v->x || ny != v->y) {
                v->x = nx; v->y = ny;
                v->layer.move(nx, ny);           // 只发 setPosition
                for (View *c : v->subs) if (c->mapped) c->place();
                static int mvdbg = 0;
                if ((mvdbg++ % 20) == 0) printf("[move] %d,%d (finger %d,%d)\n", nx, ny, fx, fy);
            }
        }
        g_in.pending = 0;
        return;
    }

    // ---------- 起手：决定 owner ----------
    if (g_owner == OWNER_NONE && g_in.pending > 0) {
        {   // 跟踪：这一帧要按下了，把命中结果和窗口几何一起记下来
            View *thv = hit_test(fx, fy);
            trace("I press screen=(%d,%d) hit=%s %p rect=(%d,%d %dx%d) nf=%d owner=%d snap=%d",
                  fx, fy,
                  thv ? (thv->title[0] ? thv->title : "view") : "(none)", (void *)thv,
                  thv ? thv->x : 0, thv ? thv->y : 0, thv ? thv->bw : 0, thv ? thv->bh : 0,
                  nf, (int)g_owner, (int)g_snap_on);
        }
        g_in.pending = 0;
        // 坐标标定用：原始值 vs 换算后的屏幕值
        printf("[tap] raw=(%d,%d) -> screen=(%d,%d)  raw范围 x[%d,%d] y[%d,%d]\n",
               g_in.slots[g_primary_slot].x, g_in.slots[g_primary_slot].y, fx, fy,
               g_in.xmin, g_in.xmax, g_in.ymin, g_in.ymax);
        View *hv = hit_test(fx, fy);
        int corner = corner_hit(hv, fx, fy);
        // 角带*不再*一按就 resize：先布防，移动超阈值才真开始拖。
        // 否则窗口底部每 96px 内都是“拖拽区”，那里的按钮永远点不动。
        g_arm_corner = 0;
        if (corner) {
            g_arm_corner = corner;
            g_arm_x = fx;
            g_arm_y = fy;
            printf("[wm] 角带布防 corner=%d '%s' (%dx%d)，移动超 %dpx 才算拖拽\n",
                   corner, hv->title[0] ? hv->title : "view", hv->bw, hv->bh,
                   24 * (g_scale > 0 ? g_scale : 1));
        }
        g_owner = OWNER_CLIENT;
        g_client_view = hv;
        g_press_x = fx; g_press_y = fy;
        g_prev_primary = g_primary_slot;

        // 顶部标题条：直接拖窗口（传统桌面隐喻），内容区完全还给客户端
        if (hv && !hv->is_subsurface && !hv->is_cursor && in_title(hv, fx, fy)) {
            g_owner = OWNER_MOVE;
            g_gesture_view = hv;
            g_move_started = false;
            g_snap_bits = 0;
            g_in_drag = true;
            g_in.grab_off_x = hv->x - fx;
            g_in.grab_off_y = hv->y - fy;
            g_in.last_cx = fx; g_in.last_cy = fy;
            raise_view(hv);
            kb_set_focus(hv);
            printf("[wm] 拖标题条 -> 移动 '%s'\n", hv->title[0] ? hv->title : "view");
            return;
        }

        g_in.down = true;
        input_button(true);
        // 布防长按：按住 650ms 不动 -> 进入“预备移动”。
        // 原来 400ms 太短：小按钮要按稳，很容易在点中之前就变成移动窗口。
        if (g_lp_timer && !g_arm_corner) wl_event_source_timer_update(g_lp_timer, 650);
        return;
    }

    // ---------- owner: CLIENT ----------
    g_in.pending = 0;

    // 手指已经开始移动 -> 撤销长按，否则 400ms 后会把正在进行的拖拽/滚动劫持成移动窗口
    if (g_lp_timer && (abs(fx - g_press_x) > 8 * (g_scale > 0 ? g_scale : 1) ||
                       abs(fy - g_press_y) > 8 * (g_scale > 0 ? g_scale : 1)))
        wl_event_source_timer_update(g_lp_timer, 0);

    // 角带已布防：手指移动超过阈值 -> 把刚才发给客户端的按压撤掉，转成 resize
    if (g_arm_corner && g_client_view && g_client_view->mapped) {
        View *v = g_client_view;
        const int32_t thr = 24 * (g_scale > 0 ? g_scale : 1);
        const int32_t adx = fx - g_arm_x, ady = fy - g_arm_y;
        if (adx * adx + ady * ady >= thr * thr) {
            if (g_in.down) { g_in.down = false; input_button(false); }  // 撤销点击
            g_owner = OWNER_RESIZE;
            g_gesture_view = v;
            g_in.resize_corner = g_arm_corner;
            g_in.resize_x0 = g_arm_x;  g_in.resize_y0 = g_arm_y;   // ← 手指起点（删了它就会乱飞）
            g_in.resize_w0 = v->bw;    g_in.resize_h0 = v->bh;
            g_in.resize_left = v->x;   g_in.resize_top = v->y;
            g_snap_bits = 0;           // 缩放期间不吸附
            printf("[wm] 角带拖拽开始 -> resize '%s' corner=%d\n",
                   v->title[0] ? v->title : "view", g_arm_corner);
            g_arm_corner = 0;
            return;
        }
    }

    // 双指进来 → 转成「改窗口大小」（传统桌面隐喻：捏窗口 = 调整它，不是给客户端）
    if (nf >= 2 && g_owner == OWNER_CLIENT && g_client_view && g_client_view->mapped) {
        View *v = g_client_view;
        int32_t ax = 0, ay = 0, bx = 0, by = 0, got = 0;
        for (int i = 0; i < 10 && got < 2; i++) {
            if (g_in.slots[i].id < 0) continue;
            int32_t sx, sy;
            raw_to_screen(g_in.slots[i].x, g_in.slots[i].y, &sx, &sy);
            if (got == 0) { ax = sx; ay = sy; got = 1; }
            else { bx = sx; by = sy; got = 2; }
        }
        int32_t dx = bx - ax, dy = by - ay;
        int32_t d0 = (int32_t)sqrt((double)(dx * dx + dy * dy));
        if (d0 > 40) {
            if (g_in.down) { g_in.down = false; input_button(false); }
            sec_release_all();
            g_owner = OWNER_PINCH;
        g_cfg_min_interval = 250;   // 捏合期间别每帧 reconfigure，否则整屏闪
            g_gesture_view = v;
            g_pinch_d0 = d0;
            g_snap_bits = 0;      // 缩放期间不吸附，否则两者互相拉扯 = 乱飞
            // 记**逻辑**尺寸做基准（物理 / buf_scale），否则 scale=3 下会被放大 3 倍
            const int32_t psc = v->buf_scale > 0 ? v->buf_scale : 1;
            g_pinch_sc = psc;                     // 冻结，整个手势期间都用它
            // 基准用「当前视觉尺寸」（含还没落实的 zoom）：
            //   用 req_w 会在“上一轮 configure 客户端没照做”时算错基准 → 一捏就跳。
            g_pinch_zoom0 = v->zoom > 0.f ? v->zoom : 1.f;
            g_pinch_w0 = (int32_t)(v->bw * g_pinch_zoom0) / psc;
            g_pinch_h0 = (int32_t)(v->bh * g_pinch_zoom0) / psc;
            // 记下两指中点与窗口原点：缩放时让中点下的内容留在原地
            g_pinch_mx = (ax + bx) / 2;
            g_pinch_my = (ay + by) / 2;
            g_pinch_px = v->x;
            g_pinch_py = v->y;
            printf("[wm] 双指 -> 缩放 '%s' 起始 %dx%d 逻辑 指距 %d\n",
                   v->title[0] ? v->title : "view", g_pinch_w0, g_pinch_h0, d0);
            return;
        }
    }

    if (nf >= 2 && g_lp_timer) wl_event_source_timer_update(g_lp_timer, 0);   // 双指 = 一定是客户端
    // 主指换了人：把 id 0 这根“逻辑主指”先抬起再重新按下，客户端的单指状态才不会卡住
    if (g_in.down && g_prev_primary >= 0 && g_primary_slot != g_prev_primary) {
        g_in.down = false; input_button(false);
        g_in.down = true;  input_button(true);
    }
    g_prev_primary = g_primary_slot;

    pointer_motion(fx, fy, g_in.down);
    if (g_in.down) input_touch_motion();
    sec_sync(g_client_view ? g_client_view : g_srv.pointer_focus);
}


static int on_input_fd(int fd, uint32_t, void *) {
    struct input_event ev[64];
    int n = (int)read(fd, ev, sizeof(ev));
    if (n <= 0) return 0;
    n /= (int)sizeof(struct input_event);

    for (int i = 0; i < n; i++) {
        if (ev[i].type == EV_ABS) {
            if (ev[i].code == ABS_MT_SLOT) {
                int32_t s = ev[i].value;
                g_in.cur_slot = (s < 0) ? 0 : (s > 9 ? 9 : s);
            } else if (ev[i].code == ABS_MT_TRACKING_ID) {
                g_in.slots[g_in.cur_slot].id = ev[i].value;
                g_in.pending = (ev[i].value < 0) ? -1 : 1;
            } else if (ev[i].code == ABS_MT_POSITION_X) {
                g_in.slots[g_in.cur_slot].x = ev[i].value;
            } else if (ev[i].code == ABS_MT_POSITION_Y) {
                g_in.slots[g_in.cur_slot].y = ev[i].value;
            }
        } else if (ev[i].type == EV_SYN && ev[i].code == SYN_REPORT) {
            handle_frame();
        }
    }
    return 0;
}

static int open_touch_device(bool grab) {
    for (int i = 0; i < 32; i++) {
        char path[64], name[96] = {0};
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;

        unsigned long abits[(ABS_MAX / (8 * sizeof(unsigned long))) + 2] = {0};
        if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abits)), abits) < 0) { close(fd); continue; }
        auto has = [&](int code) {
            return (abits[code / (8 * sizeof(unsigned long))] >> (code % (8 * sizeof(unsigned long)))) & 1ul;
        };
        if (!has(ABS_MT_POSITION_X) || !has(ABS_MT_POSITION_Y)) { close(fd); continue; }

        struct input_absinfo ai;
        if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ai) < 0) { close(fd); continue; }
        g_in.xmin = ai.minimum; g_in.xmax = ai.maximum;
        if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ai) < 0) { close(fd); continue; }
        g_in.ymin = ai.minimum; g_in.ymax = ai.maximum;
        for (int k = 0; k < 10; k++) {
            g_in.slots[k].id = -1;
            g_in.slots[k].x = g_in.xmin;
            g_in.slots[k].y = g_in.ymin;
        }
        ioctl(fd, EVIOCGNAME(sizeof name), name);

        if (grab && ioctl(fd, EVIOCGRAB, 1) == 0) printf("[input] 已独占 (EVIOCGRAB)\n");
        g_in.fd = fd;
        printf("[input] %s '%s' x[%d,%d] y[%d,%d]\n", path, name,
               g_in.xmin, g_in.xmax, g_in.ymin, g_in.ymax);
        return fd;
    }
    return -1;
}




// ---------------------------------------------------------------- 小型扩展合集
// 这几个都是客户端「探测存在性」就决定行为的扩展：实现了就等于告诉它
// "我是正经合成器"。行为本身可以很轻。

// ============ wp_content_type_v1（视频/游戏内容类型，用来切色彩/时序策略）============
static void ct_surface_set_type(wl_client *, wl_resource *r, uint32_t type) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    v->content_type = type;
}
static void ct_surface_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct wp_content_type_v1_interface ct_surface_impl = {
    .destroy         = ct_surface_destroy,
    .set_content_type = ct_surface_set_type,
};

static void ct_get_surface(wl_client *c, wl_resource *res, uint32_t id, wl_resource *surface) {
    View *v = view_from_surface(surface);
    if (!v) {
        wl_resource_post_error(res, 0, "content_type on unknown surface");
        return;
    }
    wl_resource *r = wl_resource_create(c, &wp_content_type_v1_interface, wl_resource_get_version(res), id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, &ct_surface_impl, v, nullptr);
}

static void ct_manager_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct wp_content_type_manager_v1_interface ct_manager_impl = {
    .destroy     = ct_manager_destroy,
    .get_surface_content_type = ct_get_surface,
};

static void bind_content_type(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 1) version = 1;
    wl_resource *r = wl_resource_create(c, &wp_content_type_manager_v1_interface, version, id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, &ct_manager_impl, nullptr, nullptr);
}

// ============ wp_alpha_modifier_v1（整表面透明度乘子）============
static void am_surface_set_multiplier(wl_client *, wl_resource *r, uint32_t factor) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    // 32 位定点：0xFFFFFFFF == 1.0，我们内部用 0..256
    v->alpha_mult = (int32_t)(((uint64_t)factor * 256ull) / 0xFFFFFFFFull);
    if (v->alpha_mult < 0) v->alpha_mult = 0;
    if (v->alpha_mult > 256) v->alpha_mult = 256;
}
static void am_surface_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct wp_alpha_modifier_surface_v1_interface am_surface_impl = {
    .destroy        = am_surface_destroy,
    .set_multiplier = am_surface_set_multiplier,
};

static void am_get_surface(wl_client *c, wl_resource *res, uint32_t id, wl_resource *surface) {
    View *v = view_from_surface(surface);
    if (!v) {
        wl_resource_post_error(res, 0, "alpha_modifier on unknown surface");
        return;
    }
    wl_resource *r = wl_resource_create(c, &wp_alpha_modifier_surface_v1_interface, wl_resource_get_version(res), id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, &am_surface_impl, v, nullptr);
}

static void am_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct wp_alpha_modifier_v1_interface am_impl = {
    .destroy     = am_destroy,
    .get_surface = am_get_surface,
};

static void bind_alpha_modifier(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 1) version = 1;
    wl_resource *r = wl_resource_create(c, &wp_alpha_modifier_v1_interface, version, id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, &am_impl, nullptr, nullptr);
}

// ============ wp_fifo_v1（显式排队/等待栅栏）============
// 我们本来就是「提交即拷帧」，天然按顺序执行，所以栅栏立刻放行即可。
static void fifo_set_barrier(wl_client *, wl_resource *) {}
static void fifo_wait_barrier(wl_client *, wl_resource *) {}
static void fifo_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct wp_fifo_v1_interface fifo_impl = {
    .destroy      = fifo_destroy,
    .set_barrier  = fifo_set_barrier,
    .wait_barrier = fifo_wait_barrier,
};

static void fifo_get(wl_client *c, wl_resource *res, uint32_t id, wl_resource *surface) {
    if (!view_from_surface(surface)) {
        wl_resource_post_error(res, 0, "fifo on unknown surface");
        return;
    }
    wl_resource *r = wl_resource_create(c, &wp_fifo_v1_interface, wl_resource_get_version(res), id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, &fifo_impl, nullptr, nullptr);
}

static void fifo_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct wp_fifo_manager_v1_interface fifo_mgr_impl = {
    .destroy  = fifo_mgr_destroy,
    .get_fifo = fifo_get,
};

static void bind_fifo(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 1) version = 1;
    wl_resource *r = wl_resource_create(c, &wp_fifo_manager_v1_interface, version, id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, &fifo_mgr_impl, nullptr, nullptr);
}

// ============ zwp_pointer_gestures_v1（触控板手势对象）============
// 真发事件需要触控板多点输入，这里先把对象建出来，客户端不至于因为
// "缺扩展"而关掉手势支持。
static void gest_obj_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

// 这三个接口只有 destroy 一个请求，wayland-scanner 不给它们生成 requests 结构，
// 而且 xyz_interface 这个名字已被 wl_interface 占用 → 自己起名，按 void* 传进去。
struct gest_iface_impl {
    void (*destroy)(struct wl_client *client, struct wl_resource *resource);
};
static const struct gest_iface_impl gest_impl = { .destroy = gest_obj_destroy };

static void pg_get_swipe(wl_client *c, wl_resource *res, uint32_t id, wl_resource *pointer) {
    wl_resource *r = wl_resource_create(c, &zwp_pointer_gesture_swipe_v1_interface, wl_resource_get_version(res), id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, (const void *)&gest_impl, nullptr, nullptr);
}
static void pg_get_pinch(wl_client *c, wl_resource *res, uint32_t id, wl_resource *pointer) {
    wl_resource *r = wl_resource_create(c, &zwp_pointer_gesture_pinch_v1_interface, wl_resource_get_version(res), id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, (const void *)&gest_impl, nullptr, nullptr);
}
static void pg_get_hold(wl_client *c, wl_resource *res, uint32_t id, wl_resource *pointer) {
    wl_resource *r = wl_resource_create(c, &zwp_pointer_gesture_hold_v1_interface, wl_resource_get_version(res), id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, (const void *)&gest_impl, nullptr, nullptr);
}
static void pg_release(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void pg_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

// 同样：这个协议整个生成头里都没有 requests 结构 → 自己声明。
// 成员顺序必须严格照 XML 的 request 声明顺序（opcode 就是下标）。
struct pg_iface_impl {
    void (*get_swipe_gesture)(struct wl_client *, struct wl_resource *, uint32_t, struct wl_resource *);
    void (*get_pinch_gesture)(struct wl_client *, struct wl_resource *, uint32_t, struct wl_resource *);
    void (*get_hold_gesture)(struct wl_client *, struct wl_resource *, uint32_t, struct wl_resource *);
    void (*release)(struct wl_client *, struct wl_resource *);
    void (*destroy)(struct wl_client *, struct wl_resource *);
};
static const struct pg_iface_impl pg_impl = {
    .get_swipe_gesture = pg_get_swipe,
    .get_pinch_gesture = pg_get_pinch,
    .get_hold_gesture  = pg_get_hold,
    .release           = pg_release,
    .destroy           = pg_destroy,
};

static void bind_pointer_gestures(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 3) version = 3;
    wl_resource *r = wl_resource_create(c, &zwp_pointer_gestures_v1_interface, version, id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, (const void *)&pg_impl, nullptr, nullptr);
}

// ============ wp_pointer_warp_v1（把指针瞬移到指定 surface 坐标）============
static int32_t g_pointer_x_abs = 0, g_pointer_y_abs = 0;   // 见已有的 pointer_x/y
// 签名：warp_pointer(pointer, surface, x, y, serial)
static void warp_warp(wl_client *, wl_resource *r, wl_resource *pointer, wl_resource *surface,
                      wl_fixed_t x, wl_fixed_t y, uint32_t serial) {
    (void)pointer;
    (void)serial;
    View *v = view_from_surface(surface);
    if (!v) return;
    // 视口内坐标 → 屏幕坐标（按 buffer_scale 放大）
    const double lx = wl_fixed_to_double(x), ly = wl_fixed_to_double(y);
    const int32_t sc = v->buf_scale > 0 ? v->buf_scale : 1;
    g_cur_x = v->x + (int32_t)(lx * sc);
    g_cur_y = v->y + (int32_t)(ly * sc);
    cursor_present();
}
static void warp_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static const struct wp_pointer_warp_v1_interface warp_impl = {
    .destroy      = warp_destroy,
    .warp_pointer = warp_warp,
};

static void bind_pointer_warp(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 1) version = 1;
    wl_resource *r = wl_resource_create(c, &wp_pointer_warp_v1_interface, version, id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, &warp_impl, nullptr, nullptr);
}


// ---------------------------------------------------------------- wp_linux_drm_syncobj_manager_v1
// 真显式同步：客户端提交 dmabuf 时给出「acquire 时间点」，我们必须等它 signaled
// 才能读这块 buffer；读完再 signal 它给的「release 时间点」，客户端才知道能复用。
// 拿不到 syncobj 就退化成隐式同步（不阻塞），保证不会把整个合成器卡死。
static int g_sync_drm_fd = -1;

static int sync_drm_fd(void) {
    if (g_sync_drm_fd >= 0) return g_sync_drm_fd;
    const char *dev = getenv("SFWM_DRM_DEVICE");
    g_sync_drm_fd    = open(dev && *dev ? dev : "/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (g_sync_drm_fd < 0)
        fprintf(stderr, "[syncobj] 打不开 drm 节点: %s\n", strerror(errno));
    return g_sync_drm_fd;
}

struct SyncTimeline {
    uint32_t handle = 0;
};

// timeline 接口只有 destroy 一个请求 → 生成头没有 requests 结构，自己声明
struct sync_timeline_iface_impl {
    void (*destroy)(struct wl_client *client, struct wl_resource *resource);
};
static void stl_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct sync_timeline_iface_impl stl_impl = { .destroy = stl_destroy };
static void stl_resource_destroyed(wl_resource *r) { delete (SyncTimeline *)wl_resource_get_user_data(r); }

static void sync_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static void sync_import_timeline(wl_client *c, wl_resource *res, uint32_t id, int32_t fd) {
    wl_resource *r = wl_resource_create(c, &wp_linux_drm_syncobj_timeline_v1_interface, wl_resource_get_version(res), id);
    if (!r) {
        close(fd);
        wl_client_post_no_memory(c);
        return;
    }

    const int drmfd = sync_drm_fd();
    auto     *tl    = new SyncTimeline();

    if (drmfd < 0 || drmSyncobjFDToHandle(drmfd, fd, &tl->handle) != 0) {
        printf("[syncobj] import_timeline 失败（drv 不支持 timeline syncobj？）\n");
        close(fd);
        delete tl;
        wl_resource_post_error(res, 0, "failed to import timeline");
        wl_resource_destroy(r);
        return;
    }
    close(fd);
    wl_resource_set_implementation(r, (const void *)&stl_impl, tl, stl_resource_destroyed);
    printf("[syncobj] timeline imported, handle=%u\n", tl->handle);
}

static void sync_surf_destroy(wl_client *, wl_resource *r) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (v) v->syncobj_surface = nullptr;
    wl_resource_destroy(r);
}

static void sync_set_points(View *v, bool release, wl_resource *timeline, uint32_t hi, uint32_t lo) {
    if (!v || !timeline) return;
    SyncTimeline *tl = (SyncTimeline *)wl_resource_get_user_data(timeline);
    if (!tl) return;
    const uint64_t point = ((uint64_t)hi << 32) | lo;
    if (release) {
        v->sync_rel_handle = tl->handle;
        v->sync_rel_point  = point;
        v->sync_has_rel    = true;
    } else {
        v->sync_acq_handle = tl->handle;
        v->sync_acq_point  = point;
        v->sync_has_acq    = true;
    }
}

static void sync_set_acquire(wl_client *, wl_resource *r, wl_resource *timeline, uint32_t hi, uint32_t lo) {
    sync_set_points((View *)wl_resource_get_user_data(r), false, timeline, hi, lo);
}

static void sync_set_release(wl_client *, wl_resource *r, wl_resource *timeline, uint32_t hi, uint32_t lo) {
    sync_set_points((View *)wl_resource_get_user_data(r), true, timeline, hi, lo);
}

static const struct wp_linux_drm_syncobj_surface_v1_interface sync_surf_impl = {
    .destroy           = sync_surf_destroy,
    .set_acquire_point = sync_set_acquire,
    .set_release_point = sync_set_release,
};

static void sync_get_surface(wl_client *c, wl_resource *res, uint32_t id, wl_resource *surface) {
    View *v = view_from_surface(surface);
    if (!v) {
        wl_resource_post_error(res, 0, "syncobj on unknown surface");
        return;
    }
    wl_resource *r = wl_resource_create(c, &wp_linux_drm_syncobj_surface_v1_interface, wl_resource_get_version(res), id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    v->syncobj_surface = r;
    wl_resource_set_implementation(r, &sync_surf_impl, v, nullptr);
}

static const struct wp_linux_drm_syncobj_manager_v1_interface sync_mgr_impl = {
    .destroy         = sync_mgr_destroy,
    .get_surface     = sync_get_surface,
    .import_timeline = sync_import_timeline,
};

// 探一下驱动到底支不支持 timeline syncobj。
// 注意：本机 msm_drm（Adreno）两个节点都是 SYNCOBJ=0 / TIMELINE=0，
// drmSyncobjCreate 直接 EOPNOTSUPP —— 驱动没开这个特性，内核 core 有也没用。
// 支持才宣告，否则客户端会以为能用然后卡在等待上。
static bool syncobj_supported(void) {
    const int fd = sync_drm_fd();
    if (fd < 0) return false;

    uint64_t cap = 0;
    if (drmGetCap(fd, DRM_CAP_SYNCOBJ, &cap) != 0 || !cap) return false;
    if (drmGetCap(fd, DRM_CAP_SYNCOBJ_TIMELINE, &cap) != 0 || !cap) return false;

    uint32_t so = 0;
    if (drmSyncobjCreate(fd, 0, &so) != 0) return false;
    drmSyncobjDestroy(fd, so);
    return true;
}

static void bind_syncobj(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 1) version = 1;
    wl_resource *r = wl_resource_create(c, &wp_linux_drm_syncobj_manager_v1_interface, version, id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, &sync_mgr_impl, nullptr, nullptr);
}

// blit 前等 acquire 点，blit 后 signal release 点
static void sync_wait_acquire(View *v) {
    if (!v || !v->sync_has_acq) return;
    const int drmfd = sync_drm_fd();
    if (drmfd < 0) return;
    uint32_t handles[1] = {v->sync_acq_handle};
    uint64_t points[1]  = {v->sync_acq_point};
    // 最多等 100ms：客户端异常时宁可画个旧帧也不能把事件循环卡死
    const int ret = drmSyncobjTimelineWait(drmfd, handles, points, 1, 100ull * 1000ull * 1000ull,
                                          DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, nullptr);
    if (ret != 0) {
        static int warn = 0;
        if (warn++ < 3)
            printf("[syncobj] acquire 等待超时/失败 (point=%llu): %s\n",
                   (unsigned long long)v->sync_acq_point, strerror(errno));
    }
    v->sync_has_acq = false;
}

static void sync_signal_release(View *v) {
    if (!v || !v->sync_has_rel) return;
    const int drmfd = sync_drm_fd();
    if (drmfd < 0) return;
    uint32_t handles[1] = {v->sync_rel_handle};
    uint64_t points[1]  = {v->sync_rel_point};
    if (drmSyncobjTimelineSignal(drmfd, handles, points, 1) != 0) {
        static int warn = 0;
        if (warn++ < 3)
            printf("[syncobj] signal release 失败 (point=%llu): %s\n",
                   (unsigned long long)v->sync_rel_point, strerror(errno));
    }
    v->sync_has_rel = false;
}

// ---------------------------------------------------------------- wp_presentation
// 客户端靠它拿到「这一帧什么时候上屏」。不给的话很多合成器/Hyprland 会退化到
// 自己猜时序，帧率调度会抖。我们目前按 60Hz 报，时间戳用 CLOCK_MONOTONIC。
static uint64_t pres_refresh_ns(void) {
    return 16666666ull;
}

static void pres_fb_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

// wayland-scanner 没给 wp_presentation_feedback 生成 requests 结构
// （该接口只有 destroy 一个请求），自己声明一份，布局与生成头一致。
struct pres_fb_iface_impl {
    void (*destroy)(struct wl_client *client, struct wl_resource *resource);
};
static const struct pres_fb_iface_impl pres_fb_impl = {
    .destroy = pres_fb_destroy,
};

// 事件也直接 post，不依赖生成的 send_* 辅助函数
static void pres_fb_post_discarded(wl_resource *fr) {
    wl_resource_post_event(fr, 1 /* discarded */);
}
static void pres_fb_post_presented(wl_resource *fr, uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec,
                                   uint32_t refresh, uint32_t seq_hi, uint32_t seq_lo, uint32_t flags) {
    wl_resource_post_event(fr, 0 /* presented */, sec_hi, sec_lo, nsec, refresh, seq_hi, seq_lo, flags);
}

static void pres_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static void pres_feedback(wl_client *c, wl_resource *res, wl_resource *surface, uint32_t id) {
    View *v = view_from_surface(surface);
    wl_resource *fr = wl_resource_create(c, &wp_presentation_feedback_interface, wl_resource_get_version(res), id);
    if (!fr) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(fr, (const void *)&pres_fb_impl, nullptr, nullptr);
    if (!v) {
        pres_fb_post_discarded(fr);
        wl_resource_destroy(fr);
        return;
    }
    v->feedbacks.push_back(fr);
}

static const struct wp_presentation_interface pres_impl = {
    .destroy  = pres_destroy,
    .feedback = pres_feedback,
};

static void bind_presentation(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 2) version = 2;
    wl_resource *r = wl_resource_create(c, &wp_presentation_interface, version, id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, &pres_impl, nullptr, nullptr);
}

// blit 真正把像素贴上去之后调用：把所有待回执的 feedback 一次性报掉
static void pres_send_presented(View *v) {
    if (!v || v->feedbacks.empty()) return;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    for (wl_resource *fr : v->feedbacks) {
        pres_fb_post_presented(
            fr,
            (uint32_t)((uint64_t)ts.tv_sec >> 32), (uint32_t)((uint64_t)ts.tv_sec & 0xffffffffu),
            (uint32_t)ts.tv_nsec, (uint32_t)pres_refresh_ns(),
            0, 0,
            WP_PRESENTATION_FEEDBACK_KIND_VSYNC);
        wl_resource_destroy(fr);
    }
    v->feedbacks.clear();
}

// ---------------------------------------------------------------- xdg_activation_v1
// 应用启动时用它抢焦点（没有这个，点启动器起来的窗口经常不聚焦）。
static void act_token_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void act_token_set_serial(wl_client *, wl_resource *, uint32_t, wl_resource *) {}
static void act_token_set_app_id(wl_client *, wl_resource *, const char *) {}
static void act_token_set_surface(wl_client *, wl_resource *, wl_resource *) {}

static void act_token_commit(wl_client *, wl_resource *r) {
    static uint32_t seq = 0;
    char tok[32];
    snprintf(tok, sizeof(tok), "sfwm-token-%u", ++seq);
    xdg_activation_token_v1_send_done(r, tok);
}

static const struct xdg_activation_token_v1_interface act_token_impl = {
    .set_serial   = act_token_set_serial,
    .set_app_id   = act_token_set_app_id,
    .set_surface  = act_token_set_surface,
    .commit       = act_token_commit,
    .destroy      = act_token_destroy,
};

static void act_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static void act_activate(wl_client *, wl_resource *, const char *token, wl_resource *surface) {
    View *v = surface ? view_from_surface(surface) : nullptr;
    if (!v || !v->mapped) return;

    int32_t top = 0;
    for (View *o : g_srv.views)
        if (o->mapped && !o->is_layer && !o->is_cursor && o->z > top) top = o->z;
    v->z = top + 1;
    v->place();
    kb_set_focus(v);
    printf("[activation] 激活 '%s'\n", v->title[0] ? v->title : "view");
}

static void act_get_token(wl_client *c, wl_resource *res, uint32_t id) {
    wl_resource *r = wl_resource_create(c, &xdg_activation_token_v1_interface, wl_resource_get_version(res), id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, &act_token_impl, nullptr, nullptr);
}

static const struct xdg_activation_v1_interface act_impl = {
    .destroy              = act_destroy,
    .get_activation_token = act_get_token,
    .activate             = act_activate,
};

static void bind_activation(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 1) version = 1;
    wl_resource *r = wl_resource_create(c, &xdg_activation_v1_interface, version, id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, &act_impl, nullptr, nullptr);
}

// ---------------------------------------------------------------- zwlr_layer_shell_v1
// 面板 / 托盘 / 启动器（quickshell、waybar、fuzzel、swaybg、mako）全靠它。
// 我们不做真正的「层」，只把 anchor + margin 算成屏幕坐标，然后当普通 View 上屏；
// 层序靠 z 分档：background < 普通窗口 < top < overlay。

static int32_t layer_z_for(uint32_t layer) {
    switch (layer) {
    case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND: return -3000;
    case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:     return -2000;
    case ZWLR_LAYER_SHELL_V1_LAYER_TOP:        return 500;
    case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:    return 900;
    default:                                   return 500;
    }
}

// 按 anchor/margin 算位置。尺寸优先用已到手的 buffer，其次用客户端请求值。
static void layer_apply_anchor(View *v) {
    if (!v || !v->is_layer) return;
    g_reason = "layer";

    const int32_t  ow = g_out_w, oh = g_out_h;
    const uint32_t A = v->layer_anchor;
    const bool     a_top = (A & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) != 0;
    const bool     a_bot = (A & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) != 0;
    const bool     a_lef = (A & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) != 0;
    const bool     a_rig = (A & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) != 0;

    int32_t w = v->bw > 0 ? v->bw : v->layer_w;
    int32_t h = v->bh > 0 ? v->bh : v->layer_h;
    if (w <= 0) w = (a_lef && a_rig) ? ow - v->layer_ml - v->layer_mr : ow;
    if (h <= 0) h = (a_top && a_bot) ? oh - v->layer_mt - v->layer_mb : oh;
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;

    if (a_lef && a_rig)      v->x = v->layer_ml;
    else if (a_rig)          v->x = ow - w - v->layer_mr;
    else if (a_lef)          v->x = v->layer_ml;
    else                     v->x = (ow - w) / 2;

    if (a_top && a_bot)      v->y = v->layer_mt;
    else if (a_bot)          v->y = oh - h - v->layer_mb;
    else if (a_top)          v->y = v->layer_mt;
    else                     v->y = (oh - h) / 2;

    if (v->x < 0) v->x = 0;
    if (v->y < 0) v->y = 0;
    v->place();
}

static void layer_send_configure(View *v) {
    if (!v || !v->layer_surface) return;

    const int32_t  ow = g_out_w, oh = g_out_h;
    const uint32_t A = v->layer_anchor;
    const bool     a_top = (A & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) != 0;
    const bool     a_bot = (A & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) != 0;
    const bool     a_lef = (A & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) != 0;
    const bool     a_rig = (A & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) != 0;

    int32_t w = v->layer_w, h = v->layer_h;
    if (w <= 0) w = (a_lef && a_rig) ? ow - v->layer_ml - v->layer_mr : ow;
    if (h <= 0) h = (a_top && a_bot) ? oh - v->layer_mt - v->layer_mb : oh;
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;

    zwlr_layer_surface_v1_send_configure(v->layer_surface, wl_display_next_serial(g_srv.dpy), (uint32_t)w, (uint32_t)h);
    layer_apply_anchor(v);
}

static void lsurf_set_size(wl_client *, wl_resource *r, uint32_t w, uint32_t h) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    v->layer_w = (int32_t)w;
    v->layer_h = (int32_t)h;
    layer_send_configure(v);
}

static void lsurf_set_anchor(wl_client *, wl_resource *r, uint32_t anchor) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    v->layer_anchor = anchor;
    layer_send_configure(v);
}

static void lsurf_set_exclusive_zone(wl_client *, wl_resource *r, int32_t zone) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    v->layer_excl = zone;
}

static void lsurf_set_margin(wl_client *, wl_resource *r, int32_t t, int32_t rr, int32_t b, int32_t l) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    v->layer_mt = t;
    v->layer_mr = rr;
    v->layer_mb = b;
    v->layer_ml = l;
    layer_send_configure(v);
}

static void lsurf_set_keyboard_interactivity(wl_client *, wl_resource *r, uint32_t kb) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    v->layer_kb = kb;
    if (kb != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE && v->mapped)
        kb_set_focus(v);
}

// 层表面的 popup（下拉菜单）：目前不实现真正的 grab，客户端会自己兜底
static void lsurf_get_popup(wl_client *, wl_resource *, wl_resource *) {}

static void lsurf_ack_configure(wl_client *, wl_resource *, uint32_t) {}

static void lsurf_set_layer(wl_client *, wl_resource *r, uint32_t layer) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    v->layer_num = layer;
    v->z         = layer_z_for(layer);
    if (v->mapped) v->place();
}

static void lsurf_destroy(wl_client *, wl_resource *r) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (v) v->layer_surface = nullptr;
    wl_resource_destroy(r);
}

static const struct zwlr_layer_surface_v1_interface lsurf_impl = {
    .set_size                 = lsurf_set_size,
    .set_anchor               = lsurf_set_anchor,
    .set_exclusive_zone       = lsurf_set_exclusive_zone,
    .set_margin               = lsurf_set_margin,
    .set_keyboard_interactivity = lsurf_set_keyboard_interactivity,
    .get_popup                = lsurf_get_popup,
    .ack_configure            = lsurf_ack_configure,
    .destroy                  = lsurf_destroy,
    .set_layer                = lsurf_set_layer,
};

static void lshell_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static void lshell_get_layer_surface(wl_client *c, wl_resource *res, uint32_t id, wl_resource *surface, wl_resource *output,
                                     uint32_t layer, const char *ns) {
    View *v = view_from_surface(surface);
    if (!v) {
        wl_resource_post_error(res, ZWLR_LAYER_SHELL_V1_ERROR_ROLE, "layer_surface on unknown wl_surface");
        return;
    }
    if (v->is_layer || v->xdg_surface) {
        wl_resource_post_error(res, ZWLR_LAYER_SHELL_V1_ERROR_ROLE, "surface already has a role");
        return;
    }

    wl_resource *lr = wl_resource_create(c, &zwlr_layer_surface_v1_interface, wl_resource_get_version(res), id);
    if (!lr) {
        wl_client_post_no_memory(c);
        return;
    }

    v->is_layer      = true;
    v->layer_surface = lr;
    v->layer_num     = layer;
    v->z             = layer_z_for(layer);
    snprintf(v->layer_ns, sizeof(v->layer_ns), "%s", ns ? ns : "");
    if (!v->title[0])
        snprintf(v->title, sizeof(v->title), "layer:%s", v->layer_ns);
    wl_resource_set_implementation(lr, &lsurf_impl, v, nullptr);

    printf("[layer-shell] %s layer=%u\n", v->layer_ns, layer);
    layer_send_configure(v);
}

static const struct zwlr_layer_shell_v1_interface lshell_impl = {
    .get_layer_surface = lshell_get_layer_surface,
    .destroy           = lshell_destroy,
};

static void bind_lshell(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 4) version = 4;
    wl_resource *r = wl_resource_create(c, &zwlr_layer_shell_v1_interface, version, id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, &lshell_impl, nullptr, nullptr);
}

// ---------------------------------------------------------------- xdg_wm_base
struct Positioner {
    int32_t w = 0, h = 0;
    int32_t ax = 0, ay = 0, aw = 0, ah = 0;
    int32_t ox = 0, oy = 0;
    uint32_t anchor = 0, gravity = 0;
};

static void xdg_positioner_destroy(wl_client *, wl_resource *r) {
    wl_resource_destroy(r);
}
static void xdg_positioner_set_size(wl_client *, wl_resource *r, int32_t w, int32_t h) {
    Positioner *p = (Positioner *)wl_resource_get_user_data(r);
    if (p) { p->w = w; p->h = h; }
}
static void xdg_positioner_set_anchor_rect(wl_client *, wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    Positioner *p = (Positioner *)wl_resource_get_user_data(r);
    if (p) { p->ax = x; p->ay = y; p->aw = w; p->ah = h; }
}
static void xdg_positioner_set_anchor(wl_client *, wl_resource *r, uint32_t a) {
    Positioner *p = (Positioner *)wl_resource_get_user_data(r);
    if (p) p->anchor = a;
}
static void xdg_positioner_set_gravity(wl_client *, wl_resource *r, uint32_t g) {
    Positioner *p = (Positioner *)wl_resource_get_user_data(r);
    if (p) p->gravity = g;
}
static void xdg_positioner_set_constraint_adjustment(wl_client *, wl_resource *, uint32_t) {}
static void xdg_positioner_set_offset(wl_client *, wl_resource *r, int32_t x, int32_t y) {
    Positioner *p = (Positioner *)wl_resource_get_user_data(r);
    if (p) { p->ox = x; p->oy = y; }
}
// v3 新增的三个请求；我们不做 constrain 调整，接受即可（set_reactive 在这份生成头里无参数）
static void xdg_positioner_set_reactive(wl_client *, wl_resource *) {}
static void xdg_positioner_set_parent_size(wl_client *, wl_resource *, int32_t, int32_t) {}
static void xdg_positioner_set_parent_configure(wl_client *, wl_resource *, uint32_t) {}

static const struct xdg_positioner_interface positioner_impl = {
    .destroy = xdg_positioner_destroy,
    .set_size = xdg_positioner_set_size,
    .set_anchor_rect = xdg_positioner_set_anchor_rect,
    .set_anchor = xdg_positioner_set_anchor,
    .set_gravity = xdg_positioner_set_gravity,
    .set_constraint_adjustment = xdg_positioner_set_constraint_adjustment,
    .set_offset = xdg_positioner_set_offset,
    .set_reactive = xdg_positioner_set_reactive,
    .set_parent_size = xdg_positioner_set_parent_size,
    .set_parent_configure = xdg_positioner_set_parent_configure,
};

static void toplevel_destroy(wl_client *, wl_resource *r) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (v) v->toplevel = nullptr;
    wl_resource_destroy(r);
}
static void toplevel_set_parent(wl_client *, wl_resource *, wl_resource *) {}
static void toplevel_set_title(wl_client *, wl_resource *r, const char *t) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (v && t) { snprintf(v->title, sizeof(v->title), "%s", t); }
}
static void toplevel_set_app_id(wl_client *, wl_resource *r, const char *a) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (v && a) { snprintf(v->app_id, sizeof(v->app_id), "%s", a); }
}
static void toplevel_show_window_menu(wl_client *, wl_resource *, wl_resource *, uint32_t, int32_t, int32_t) {}
static void toplevel_move(wl_client *, wl_resource *r, wl_resource *, uint32_t) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    // 客户端自己发起的移动（有标题栏的客户端才会走这里）：交给输入层做交互式拖拽
    g_srv.move_view = v;
    printf("[input] 客户端请求移动: %s\n", v->title[0] ? v->title : "view");
}
static void toplevel_resize(wl_client *, wl_resource *, wl_resource *, uint32_t, uint32_t) {}
static void toplevel_set_max_size(wl_client *, wl_resource *, int32_t, int32_t) {}
static void toplevel_set_min_size(wl_client *, wl_resource *, int32_t, int32_t) {}

// 客户端实际渲染比例（buffer 像素 / 逻辑像素）。
//   优先用客户端明确上报的 buf_scale；但很多客户端（Qt/OBS）根本不调
//   set_buffer_scale，只按 wl_output 的 scale 放大 buffer —— 此时 buf_scale 恒为 1。
//   若拿它去除屏宽，configure 里就发 1272 逻辑宽，客户端给出 2542 的 buffer，
//   窗口比屏幕还宽、右边整条被裁掉（底部按钮永远点不到）。
//   第二轮开始用上一轮实测的 bw/req_w 反推，所以即使第一次猜错也能自愈。
static int32_t client_scale(View *v) {
    if (!v) return g_scale > 0 ? g_scale : 1;
    if (v->buf_scale > 1) return v->buf_scale;
    if (v->req_w > 0 && v->bw > 0) {
        int32_t r = (int32_t)(((int64_t)v->bw + v->req_w / 2) / v->req_w);
        if (r < 1) r = 1;
        if (r > 4) r = 4;
        return r;
    }
    return g_scale > 0 ? g_scale : 1;
}

static void toplevel_send_configure(View *v) {
    // 窗口可能正在销毁（点“关闭窗口”时 xdg_toplevel 先没了），不拦就会段错
    if (!v || !v->toplevel || !v->xdg_surface) return;
    struct wl_array states;
    wl_array_init(&states);
    uint32_t *a = (uint32_t *)wl_array_add(&states, 4 * sizeof(uint32_t));
    int n = 0;
    if (a) {
        a[n++] = XDG_TOPLEVEL_STATE_ACTIVATED;
        if (v->want_maximized) a[n++] = XDG_TOPLEVEL_STATE_MAXIMIZED;
        if (v->want_fullscreen) a[n++] = XDG_TOPLEVEL_STATE_FULLSCREEN;
    }

    // 关键：不要给每个窗口都塞整屏尺寸，否则对话框会被拉满。0x0 = 客户端自己决定。
    int32_t lw = 0, lh = 0;
    const int32_t cfgScale = client_scale(v);   // 实测出来的客户端比例
    if (v->want_maximized || v->want_fullscreen) {
        lw = g_out_w / cfgScale;
        lh = g_out_h / cfgScale;
        if (lw < 1) lw = 1;
        if (lh < 1) lh = 1;
    } else if (v->req_w > 0 && v->req_h > 0) {
        // 拖边角指定的尺寸优先级高于“客户端自己决定”
        lw = v->req_w;
        lh = v->req_h;
    }

    xdg_toplevel_send_configure(v->toplevel, lw, lh, &states);
    wl_array_release(&states);

    const int tver = wl_resource_get_version(v->toplevel);
    // v4：告诉客户端最大可用尺寸
    if (tver >= 4) {
        int32_t mw = g_out_w / cfgScale;
        int32_t mh = g_out_h / cfgScale;
        xdg_toplevel_send_configure_bounds(v->toplevel, mw, mh);
    }
    // v5：WM 能力位（窗口菜单/最大化/全屏）
    if (tver >= 5) {
        struct wl_array caps;
        wl_array_init(&caps);
        const uint32_t vals[3] = {
            XDG_TOPLEVEL_WM_CAPABILITIES_WINDOW_MENU,
            XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE,
            XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN,
        };
        for (int i = 0; i < 3; i++) {
            uint32_t *c = (uint32_t *)wl_array_add(&caps, sizeof(uint32_t));
            if (c) *c = vals[i];
        }
        xdg_toplevel_send_wm_capabilities(v->toplevel, &caps);
        wl_array_release(&caps);
    }

    xdg_surface_send_configure(v->xdg_surface, wl_display_next_serial(g_srv.dpy));
}

static void toplevel_set_maximized(wl_client *, wl_resource *r) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    v->want_maximized = true;
    v->x = 0; v->y = 0;
    if (v->mapped) v->place();
    toplevel_send_configure(v);
}
static void toplevel_unset_maximized(wl_client *, wl_resource *r) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    v->want_maximized = false;
    toplevel_send_configure(v);
}
static void toplevel_set_fullscreen(wl_client *, wl_resource *r, wl_resource *) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    v->want_fullscreen = true;
    v->x = 0; v->y = 0;
    if (v->mapped) v->place();
    toplevel_send_configure(v);
}
static void toplevel_unset_fullscreen(wl_client *, wl_resource *r) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    v->want_fullscreen = false;
    toplevel_send_configure(v);
}
static void toplevel_set_minimized(wl_client *, wl_resource *r) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (v && v->layer.win) v->layer.hide();
}

static const struct xdg_toplevel_interface toplevel_impl = {
    .destroy = toplevel_destroy,
    .set_parent = toplevel_set_parent,
    .set_title = toplevel_set_title,
    .set_app_id = toplevel_set_app_id,
    .show_window_menu = toplevel_show_window_menu,
    .move = toplevel_move,
    .resize = toplevel_resize,
    .set_max_size = toplevel_set_max_size,
    .set_min_size = toplevel_set_min_size,
    .set_maximized = toplevel_set_maximized,
    .unset_maximized = toplevel_unset_maximized,
    .set_fullscreen = toplevel_set_fullscreen,
    .unset_fullscreen = toplevel_unset_fullscreen,
    .set_minimized = toplevel_set_minimized,
};

static void popup_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void popup_grab(wl_client *, wl_resource *, wl_resource *, uint32_t) {}
static void popup_reposition(wl_client *, wl_resource *r, wl_resource *, uint32_t token) {
    xdg_popup_send_repositioned(r, token);
}
static const struct xdg_popup_interface popup_impl = {
    .destroy = popup_destroy,
    .grab = popup_grab,
    .reposition = popup_reposition,
};

static void xdg_surface_destroy(wl_client *, wl_resource *r) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (v) v->xdg_surface = nullptr;
    wl_resource_destroy(r);
}

static void xdg_surface_get_toplevel(wl_client *c, wl_resource *r, uint32_t id) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    wl_resource *tr = wl_resource_create(c, &xdg_toplevel_interface,
                                         wl_resource_get_version(r), id);
    if (!tr) { wl_client_post_no_memory(c); return; }
    v->toplevel = tr;
    wl_resource_set_implementation(tr, &toplevel_impl, v, nullptr);
    toplevel_send_configure(v);
}

static void xdg_surface_get_popup(wl_client *c, wl_resource *r, uint32_t id,
                                 wl_resource *parent_res, wl_resource *pos_res) {
    View *v = (View *)wl_resource_get_user_data(r);
    if (!v) return;
    View *pv = parent_res ? (View *)wl_resource_get_user_data(parent_res) : nullptr;
    Positioner *pos = pos_res ? (Positioner *)wl_resource_get_user_data(pos_res) : nullptr;

    wl_resource *pr = wl_resource_create(c, &xdg_popup_interface, wl_resource_get_version(r), id);
    if (!pr) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(pr, &popup_impl, v, nullptr);

    // 弹窗必须收到 configure 才会映射——没发它就是“菜单不出来”
    int32_t lw = (pos && pos->w > 0) ? pos->w : (v->bw > 0 ? v->bw : 200);
    int32_t lh = (pos && pos->h > 0) ? pos->h : (v->bh > 0 ? v->bh : 200);
    int32_t lx = pos ? (pos->ax + pos->ox) : 0;
    int32_t ly = pos ? (pos->ay + pos->oy) : 0;

    v->parent = pv;
    v->is_popup = true;
    int32_t sc = pv ? pv->buf_scale : 1;
    v->x = (pv ? pv->x : 0) + lx * sc;
    v->y = (pv ? pv->y : 0) + ly * sc;

    xdg_popup_send_configure(pr, lx, ly, lw, lh);
    xdg_surface_send_configure(v->xdg_surface, wl_display_next_serial(g_srv.dpy));
    printf("[sfwm] popup '%s' 逻辑(%d,%d) %dx%d -> 物理(%d,%d)\n",
           v->title[0] ? v->title : "menu", lx, ly, lw, lh, v->x, v->y);
}

static void xdg_surface_set_window_geometry(wl_client *, wl_resource *, int32_t, int32_t, int32_t, int32_t) {}
static void xdg_surface_ack_configure(wl_client *, wl_resource *, uint32_t) {}

static const struct xdg_surface_interface xdg_surface_impl = {
    .destroy = xdg_surface_destroy,
    .get_toplevel = xdg_surface_get_toplevel,
    .get_popup = xdg_surface_get_popup,
    .set_window_geometry = xdg_surface_set_window_geometry,
    .ack_configure = xdg_surface_ack_configure,
};

static void wm_base_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

static void wm_base_create_positioner(wl_client *c, wl_resource *res, uint32_t id) {
    wl_resource *r = wl_resource_create(c, &xdg_positioner_interface,
                                        wl_resource_get_version(res), id);
    if (!r) { wl_client_post_no_memory(c); return; }
    Positioner *p = new Positioner();
    wl_resource_set_implementation(r, &positioner_impl, p, [](wl_resource *res2) {
        delete (Positioner *)wl_resource_get_user_data(res2);
    });
}

static void wm_base_get_xdg_surface(wl_client *c, wl_resource *res, uint32_t id, wl_resource *surface) {
    View *v = (View *)wl_resource_get_user_data(surface);
    if (!v) return;
    wl_resource *r = wl_resource_create(c, &xdg_surface_interface,
                                        wl_resource_get_version(res), id);
    if (!r) { wl_client_post_no_memory(c); return; }
    v->xdg_surface = r;
    wl_resource_set_implementation(r, &xdg_surface_impl, v, nullptr);
}

static void wm_base_pong(wl_client *, wl_resource *, uint32_t) {}

static const struct xdg_wm_base_interface wm_base_impl = {
    .destroy = wm_base_destroy,
    .create_positioner = wm_base_create_positioner,
    .get_xdg_surface = wm_base_get_xdg_surface,
    .pong = wm_base_pong,
};

static void bind_wm_base(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 6) version = 6;   // v5 => configure_bounds / wm_capabilities
    wl_resource *r = wl_resource_create(c, &xdg_wm_base_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &wm_base_impl, nullptr, nullptr);
}

// ---------------------------------------------------------------- 更多协议
//
// 目标：把常见客户端 / 嵌套窗管会 bind 的全局对象都提供出来。
// 大多数实现是"接受并忽略"，但只要对象存在，客户端就不会在启动时直接失败。
// 真正影响能否跑起来的是 wl_compositor / wl_shm / wl_seat / xdg-shell / wl_output，
// 这几个前面已经实现了。

// ---- wp_viewporter（缩放/裁剪；我们只记住，不改实际合成）
static void viewport_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void viewport_set_source(wl_client *, wl_resource *, wl_fixed_t, wl_fixed_t, wl_fixed_t, wl_fixed_t) {}
static void viewport_set_destination(wl_client *, wl_resource *, int32_t, int32_t) {}
static const struct wp_viewport_interface viewport_impl = {
    .destroy = viewport_destroy,
    .set_source = viewport_set_source,
    .set_destination = viewport_set_destination,
};
static void viewporter_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void viewporter_get_viewport(wl_client *c, wl_resource *res, uint32_t id, wl_resource *) {
    wl_resource *v = wl_resource_create(c, &wp_viewport_interface, wl_resource_get_version(res), id);
    if (!v) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(v, &viewport_impl, nullptr, nullptr);
}
static const struct wp_viewporter_interface viewporter_impl = {
    .destroy = viewporter_destroy,
    .get_viewport = viewporter_get_viewport,
};

// ---- wp_presentation 没接：Termux 这份生成的 server-header 里缺少
// wp_presentation_feedback_interface 的结构定义，不强行用（对客户端不是必需的）。

// ---- wp_fractional_scale_v1
static void frac_scale_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct wp_fractional_scale_v1_interface frac_scale_impl = {
    .destroy = frac_scale_destroy,
};
static void frac_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void frac_mgr_get(wl_client *c, wl_resource *res, uint32_t id, wl_resource *) {
    wl_resource *f = wl_resource_create(c, &wp_fractional_scale_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!f) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(f, &frac_scale_impl, nullptr, nullptr);
    wp_fractional_scale_v1_send_preferred_scale(f, (uint32_t)(120 * (g_scale > 0 ? g_scale : 1)));
}
static const struct wp_fractional_scale_manager_v1_interface frac_mgr_impl = {
    .destroy = frac_mgr_destroy,
    .get_fractional_scale = frac_mgr_get,
};

// ---- wp_cursor_shape_v1
static void cs_dev_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
// shape 只是个枚举（不带像素），合成器得自己按编号找图 —— 交给 cursor_build()。
static void cs_dev_set_shape(wl_client *, wl_resource *, uint32_t serial, uint32_t shape) {
    (void)serial;
    if (shape != g_cur_shape) {
        g_cur_shape = shape;
        cursor_build(shape);
        printf("[cursor] 形状 -> %u\n", shape);
    }
    if (!(g_srv.cursor_view && g_srv.cursor_view->layer.win)) cursor_present();
}
static const struct wp_cursor_shape_device_v1_interface cs_dev_impl = {
    .destroy = cs_dev_destroy,
    .set_shape = cs_dev_set_shape,
};
static void cs_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void cs_mgr_get_pointer(wl_client *c, wl_resource *res, uint32_t id, wl_resource *) {
    wl_resource *d = wl_resource_create(c, &wp_cursor_shape_device_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!d) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(d, &cs_dev_impl, nullptr, nullptr);
}
static void cs_mgr_get_tablet_tool(wl_client *c, wl_resource *res, uint32_t id, wl_resource *) {
    wl_resource *d = wl_resource_create(c, &wp_cursor_shape_device_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!d) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(d, &cs_dev_impl, nullptr, nullptr);
}
static const struct wp_cursor_shape_manager_v1_interface cs_mgr_impl = {
    .destroy = cs_mgr_destroy,
    .get_pointer = cs_mgr_get_pointer,
    .get_tablet_tool_v2 = cs_mgr_get_tablet_tool,
};

// ---- wp_single_pixel_buffer_v1（我们自己造一块 1x1 的 shm 缓冲）
static void spb_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void spb_create_u32_rgba(wl_client *c, wl_resource *res, uint32_t id,
                                uint32_t r_, uint32_t g_, uint32_t b_, uint32_t a_) {
    int fd = sfwm_rt_file("sfwm-1px", 4);
    if (fd < 0) {
        wl_client_post_no_memory(c);
        return;
    }
    void *p = mmap(nullptr, 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { close(fd); wl_client_post_no_memory(c); return; }
    // ARGB8888 小端：byte0=B, 1=G, 2=R, 3=A
    uint8_t *px = (uint8_t *)p;
    px[0] = (uint8_t)(b_ >> 24 ? b_ >> 24 : b_);
    px[1] = (uint8_t)g_;
    px[2] = (uint8_t)r_;
    px[3] = (uint8_t)a_;

    ShmPool *pool = new ShmPool();
    pool->fd = fd;
    pool->size = 4;
    pool->base = (uint8_t *)p;

    wl_resource *br = wl_resource_create(c, &wl_buffer_interface, wl_resource_get_version(res), id);
    if (!br) { shm_pool_unref(pool); wl_client_post_no_memory(c); return; }
    ShmBuf *sb = new ShmBuf();
    sb->pool = pool;
    sb->offset = 0;
    pool->refs++;
    sb->w = 1; sb->h = 1; sb->stride = 4; sb->format = WL_SHM_FORMAT_ARGB8888;
    wl_resource_set_implementation(br, &buffer_impl, sb, [](wl_resource *rr) {
        ShmBuf *bb = (ShmBuf *)wl_resource_get_user_data(rr);
        if (!bb) return;
        shm_pool_unref(bb->pool);
        delete bb;
    });
}
static const struct wp_single_pixel_buffer_manager_v1_interface spb_impl = {
    .destroy = spb_destroy,
    .create_u32_rgba_buffer = spb_create_u32_rgba,
};

// ---- wp_tearing_control_v1
static void tc_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void tc_set_hint(wl_client *, wl_resource *, uint32_t) {}
static const struct wp_tearing_control_v1_interface tc_impl = {
    .set_presentation_hint = tc_set_hint,
    .destroy = tc_destroy,
};
static void tc_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void tc_mgr_get(wl_client *c, wl_resource *res, uint32_t id, wl_resource *) {
    wl_resource *o = wl_resource_create(c, &wp_tearing_control_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!o) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(o, &tc_impl, nullptr, nullptr);
}
static const struct wp_tearing_control_manager_v1_interface tc_mgr_impl = {
    .destroy = tc_mgr_destroy,
    .get_tearing_control = tc_mgr_get,
};

// ---- zwp_relative_pointer_v1
static std::vector<wl_resource *> g_relative_ptrs;
static void rp_destroy(wl_client *, wl_resource *r) {
    for (size_t i = 0; i < g_relative_ptrs.size(); i++)
        if (g_relative_ptrs[i] == r) { g_relative_ptrs.erase(g_relative_ptrs.begin() + i); break; }
    wl_resource_destroy(r);
}
static const struct zwp_relative_pointer_v1_interface rp_impl = { .destroy = rp_destroy };
static void rp_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void rp_mgr_get(wl_client *c, wl_resource *res, uint32_t id, wl_resource *) {
    wl_resource *o = wl_resource_create(c, &zwp_relative_pointer_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!o) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(o, &rp_impl, nullptr, nullptr);
    g_relative_ptrs.push_back(o);
}
static const struct zwp_relative_pointer_manager_v1_interface rp_mgr_impl = {
    .destroy = rp_mgr_destroy,
    .get_relative_pointer = rp_mgr_get,
};

// ---- zwp_pointer_constraints_v1（锁/限制指针：直接告诉客户端"已生效"）
static void lp_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void lp_set_hint(wl_client *, wl_resource *, wl_fixed_t, wl_fixed_t) {}
static void lp_set_region(wl_client *, wl_resource *, wl_resource *) {}
static const struct zwp_locked_pointer_v1_interface lp_impl = {
    .set_cursor_position_hint = lp_set_hint,
    .set_region = lp_set_region,
    .destroy = lp_destroy,
};
static void cp_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void cp_set_region(wl_client *, wl_resource *, wl_resource *) {}
static const struct zwp_confined_pointer_v1_interface cp_impl = {
    .set_region = cp_set_region,
    .destroy = cp_destroy,
};
static void pc_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void pc_mgr_lock(wl_client *c, wl_resource *res, uint32_t id, wl_resource *,
                        wl_resource *, wl_resource *, uint32_t) {
    wl_resource *o = wl_resource_create(c, &zwp_locked_pointer_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!o) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(o, &lp_impl, nullptr, nullptr);
    zwp_locked_pointer_v1_send_locked(o);
}
static void pc_mgr_confine(wl_client *c, wl_resource *res, uint32_t id, wl_resource *,
                           wl_resource *, wl_resource *, uint32_t) {
    wl_resource *o = wl_resource_create(c, &zwp_confined_pointer_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!o) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(o, &cp_impl, nullptr, nullptr);
    zwp_confined_pointer_v1_send_confined(o);
}
static const struct zwp_pointer_constraints_v1_interface pc_mgr_impl = {
    .destroy = pc_mgr_destroy,
    .lock_pointer = pc_mgr_lock,
    .confine_pointer = pc_mgr_confine,
};

// ---- zwp_idle_inhibit_v1
static void ii_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct zwp_idle_inhibitor_v1_interface ii_impl = { .destroy = ii_destroy };
static void ii_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void ii_mgr_create(wl_client *c, wl_resource *res, uint32_t id, wl_resource *) {
    wl_resource *o = wl_resource_create(c, &zwp_idle_inhibitor_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!o) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(o, &ii_impl, nullptr, nullptr);
}
static const struct zwp_idle_inhibit_manager_v1_interface ii_mgr_impl = {
    .destroy = ii_mgr_destroy,
    .create_inhibitor = ii_mgr_create,
};

// ---- zwp_keyboard_shortcuts_inhibit_v1（告诉客户端快捷键归它）
static void ksi_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct zwp_keyboard_shortcuts_inhibitor_v1_interface ksi_impl = { .destroy = ksi_destroy };
static void ksi_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void ksi_mgr_inhibit(wl_client *c, wl_resource *res, uint32_t id, wl_resource *, wl_resource *) {
    wl_resource *o = wl_resource_create(c, &zwp_keyboard_shortcuts_inhibitor_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!o) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(o, &ksi_impl, nullptr, nullptr);
    zwp_keyboard_shortcuts_inhibitor_v1_send_active(o);
}
static const struct zwp_keyboard_shortcuts_inhibit_manager_v1_interface ksi_mgr_impl = {
    .destroy = ksi_mgr_destroy,
    .inhibit_shortcuts = ksi_mgr_inhibit,
};

// ---- zxdg_output_manager_v1
static void xo_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct zxdg_output_v1_interface xo_impl = { .destroy = xo_destroy };
static void xo_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void xo_mgr_get(wl_client *c, wl_resource *res, uint32_t id, wl_resource *output) {
    uint32_t ver = wl_resource_get_version(res);
    wl_resource *o = wl_resource_create(c, &zxdg_output_v1_interface, ver, id);
    if (!o) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(o, &xo_impl, output, nullptr);
    int32_t lw = g_out_w / (g_scale > 0 ? g_scale : 1);
    int32_t lh = g_out_h / (g_scale > 0 ? g_scale : 1);
    zxdg_output_v1_send_logical_position(o, 0, 0);
    zxdg_output_v1_send_logical_size(o, lw, lh);
    if (ver >= 2) {
        zxdg_output_v1_send_name(o, "SFWM-1");
        zxdg_output_v1_send_description(o, "sfwm output");
    }
    if (ver < 3) {
        zxdg_output_v1_send_done(o);
    } else if (output && wl_resource_get_version(output) >= 2) {
        // xdg-output v3 起 `zxdg_output_v1.done` 被删除，客户端改用 `wl_output.done`
        // 来给这一批信息收尾。（v3 规范原话：“From version 3 on, the protocol uses
        // wl_output.done”。）
        // 麻烦在于：bind_output() 里已经发过一次 done，但那是在客户端请求
        // get_xdg_output **之前**，此时它还不知道 logical_size。Qt 的 QWaylandOutput
        // 会因此认为信息不完整、屏幕建不出来，只打印
        //     qt.qpa.wayland: There are no outputs - creating placeholder screen
        // 然后套一个占位屏（尺寸错乱、UI 不绘制）。
        // ⇒ xdg 数据发完之后必须**再补一次** wl_output.done。规范允许多次 done。
        wl_output_send_done(output);
    }
}
static const struct zxdg_output_manager_v1_interface xo_mgr_impl = {
    .destroy = xo_mgr_destroy,
    .get_xdg_output = xo_mgr_get,
};

// ---- zxdg_decoration_manager_v1（我们不做服务端装饰）
static void deco_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void deco_send_mode(wl_resource *r) {
    zxdg_toplevel_decoration_v1_send_configure(r, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
}
static void deco_set_mode(wl_client *, wl_resource *r, uint32_t) { deco_send_mode(r); }
static void deco_unset_mode(wl_client *, wl_resource *r) { deco_send_mode(r); }
static const struct zxdg_toplevel_decoration_v1_interface deco_impl = {
    .destroy = deco_destroy,
    .set_mode = deco_set_mode,
    .unset_mode = deco_unset_mode,
};
static void deco_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void deco_mgr_get(wl_client *c, wl_resource *res, uint32_t id, wl_resource *) {
    wl_resource *o = wl_resource_create(c, &zxdg_toplevel_decoration_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!o) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(o, &deco_impl, nullptr, nullptr);
    deco_send_mode(o);
}
static const struct zxdg_decoration_manager_v1_interface deco_mgr_impl = {
    .destroy = deco_mgr_destroy,
    .get_toplevel_decoration = deco_mgr_get,
};

// ---- wl_data_device_manager（剪贴板 / 拖放：先只做"能 bind"）
static void dsrc_offer(wl_client *, wl_resource *, const char *) {}
static void dsrc_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void dsrc_set_actions(wl_client *, wl_resource *, uint32_t) {}
static const struct wl_data_source_interface dsrc_impl = {
    .offer = dsrc_offer,
    .destroy = dsrc_destroy,
    .set_actions = dsrc_set_actions,
};
static void ddev_start_drag(wl_client *, wl_resource *, wl_resource *, wl_resource *,
                            wl_resource *, uint32_t) {}
static void ddev_set_selection(wl_client *, wl_resource *, wl_resource *, uint32_t) {}
static void ddev_release(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct wl_data_device_interface ddev_impl = {
    .start_drag = ddev_start_drag,
    .set_selection = ddev_set_selection,
    .release = ddev_release,
};
static void ddm_create_source(wl_client *c, wl_resource *res, uint32_t id) {
    wl_resource *o = wl_resource_create(c, &wl_data_source_interface,
                                        wl_resource_get_version(res), id);
    if (!o) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(o, &dsrc_impl, nullptr, nullptr);
}
static void ddm_get_device(wl_client *c, wl_resource *res, uint32_t id, wl_resource *) {
    wl_resource *o = wl_resource_create(c, &wl_data_device_interface,
                                        wl_resource_get_version(res), id);
    if (!o) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(o, &ddev_impl, nullptr, nullptr);
}
static const struct wl_data_device_manager_interface ddm_impl = {
    .create_data_source = ddm_create_source,
    .get_data_device = ddm_get_device,
};

// ---- zwp_primary_selection_device_manager_v1
static void psel_src_offer(wl_client *, wl_resource *, const char *) {}
static void psel_src_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct zwp_primary_selection_source_v1_interface psel_src_impl = {
    .offer = psel_src_offer,
    .destroy = psel_src_destroy,
};
static void psel_dev_set_selection(wl_client *, wl_resource *, wl_resource *, uint32_t) {}
static void psel_dev_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct zwp_primary_selection_device_v1_interface psel_dev_impl = {
    .set_selection = psel_dev_set_selection,
    .destroy = psel_dev_destroy,
};
static void psel_mgr_create_source(wl_client *c, wl_resource *res, uint32_t id) {
    wl_resource *o = wl_resource_create(c, &zwp_primary_selection_source_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!o) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(o, &psel_src_impl, nullptr, nullptr);
}
static void psel_mgr_get_device(wl_client *c, wl_resource *res, uint32_t id, wl_resource *) {
    wl_resource *o = wl_resource_create(c, &zwp_primary_selection_device_v1_interface,
                                        wl_resource_get_version(res), id);
    if (!o) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(o, &psel_dev_impl, nullptr, nullptr);
}
static void psel_mgr_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct zwp_primary_selection_device_manager_v1_interface psel_mgr_impl = {
    .create_source = psel_mgr_create_source,
    .get_device = psel_mgr_get_device,
    .destroy = psel_mgr_destroy,
};

// 没状态的全局对象：用宏生成 bind 函数，省得每个协议手写一遍
#define DEFINE_BIND(NAME, IFACE, IMPL)                                            \
    static void bind_##NAME(wl_client *c, void *, uint32_t version, uint32_t id) { \
        if (version > IFACE.version) version = IFACE.version;                      \
        wl_resource *r = wl_resource_create(c, &IFACE, version, id);               \
        if (!r) { wl_client_post_no_memory(c); return; }                           \
        wl_resource_set_implementation(r, &IMPL, nullptr, nullptr);                \
    }

DEFINE_BIND(viewporter, wp_viewporter_interface, viewporter_impl)
DEFINE_BIND(fracescale, wp_fractional_scale_manager_v1_interface, frac_mgr_impl)
DEFINE_BIND(cursorshape, wp_cursor_shape_manager_v1_interface, cs_mgr_impl)
DEFINE_BIND(spbuffer, wp_single_pixel_buffer_manager_v1_interface, spb_impl)
DEFINE_BIND(tearing, wp_tearing_control_manager_v1_interface, tc_mgr_impl)
DEFINE_BIND(relptr, zwp_relative_pointer_manager_v1_interface, rp_mgr_impl)
DEFINE_BIND(ptrcon, zwp_pointer_constraints_v1_interface, pc_mgr_impl)
DEFINE_BIND(idleinhibit, zwp_idle_inhibit_manager_v1_interface, ii_mgr_impl)
DEFINE_BIND(ksinhibit, zwp_keyboard_shortcuts_inhibit_manager_v1_interface, ksi_mgr_impl)
DEFINE_BIND(xdgoutput, zxdg_output_manager_v1_interface, xo_mgr_impl)
DEFINE_BIND(decoration, zxdg_decoration_manager_v1_interface, deco_mgr_impl)
DEFINE_BIND(datadev, wl_data_device_manager_interface, ddm_impl)
DEFINE_BIND(primsel, zwp_primary_selection_device_manager_v1_interface, psel_mgr_impl)

// ---------------------------------------------------------------- xwayland-shell-v1
// Xwayland（>= 23.1）在 rootless 模式下必须能绑到 xwayland_shell_v1，
// 否则直接拒绝启动 rootless。协议本身只是给 wl_surface 打一个「这是 X 窗口」的标记，
// 合成器这边不需要发任何事件，接受请求即可。
static void xwlsurf_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void xwlsurf_set_serial(wl_client *, wl_resource *r, uint32_t lo, uint32_t hi) {
    printf("[xwl] set_serial %08x%08x (xwayland_surface#%u)\n", hi, lo, wl_resource_get_id(r));
}
static const struct xwayland_surface_v1_interface xwlsurf_impl = {
    .destroy = xwlsurf_destroy,
    .set_serial = xwlsurf_set_serial,
};

static void xwlshell_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void xwlshell_get_surface(wl_client *c, wl_resource *, uint32_t id, wl_resource *surface) {
    wl_resource *r = wl_resource_create(c, &xwayland_surface_v1_interface, 1, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &xwlsurf_impl, nullptr, nullptr);
    // 表面角色标记：把 surface 也记一下，方便日志对照
    printf("[xwl] 新 X 窗口 surface#%u -> xwayland_surface#%u\n",
           surface ? wl_resource_get_id(surface) : 0, id);
}
static const struct xwayland_shell_v1_interface xwlshell_impl = {
    .destroy = xwlshell_destroy,
    .get_xwayland_surface = xwlshell_get_surface,
};

static void bind_xwlshell(wl_client *c, void *, uint32_t version, uint32_t id) {
    if (version > 1) version = 1;
    wl_resource *r = wl_resource_create(c, &xwayland_shell_v1_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &xwlshell_impl, nullptr, nullptr);
    printf("[xwl] Xwayland 绑定了 xwayland_shell_v1\n");
}

// ---------------------------------------------------------------- main
int main(int argc, char **argv) {
    // 重定向到文件时 stdout 默认是全缓冲，段错时日志尾巴会丢 —— 强制行缓冲
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IOLBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--size=", 7)) sscanf(argv[i] + 7, "%dx%d", &g_out_w, &g_out_h);
        else if (!strncmp(argv[i], "--z=", 4)) g_base_z = atoi(argv[i] + 4);
        else if (!strncmp(argv[i], "--socket=", 9)) g_socket = argv[i] + 9;
        else if (!strncmp(argv[i], "--scale=", 8)) g_scale = atoi(argv[i] + 8);
        else if (!strcmp(argv[i], "--grab")) g_grab = true;
        else if (!strcmp(argv[i], "--clickthrough")) g_clickthrough = true;
        else if (!strcmp(argv[i], "--touch")) g_touch_enabled = true;
    }
    printf("[sfwm] 输出 %dx%d (逻辑 %dx%d), scale=%d, base_z=%d, socket=%s\n",
           g_out_w, g_out_h, g_out_w / g_scale, g_out_h / g_scale, g_scale, g_base_z, g_socket);

    sf::Lib &L = sf::Lib::get();
    if (!L.ok) { fprintf(stderr, "[sfwm] libgui 初始化失败\n"); return 1; }

    g_srv.dpy = wl_display_create();
    if (!g_srv.dpy) { fprintf(stderr, "[sfwm] wl_display_create 失败\n"); return 1; }
    g_srv.loop = wl_display_get_event_loop(g_srv.dpy);

    // 轮询模块写的输入法状态文件（300ms）：键盘高度 + 显示状态
    g_ime_timer = wl_event_loop_add_timer(g_srv.loop, on_ime_tick, nullptr);
    if (g_ime_timer) wl_event_source_timer_update(g_ime_timer, 300);

    // 长按 400ms -> 移动窗口。
    // ⚠️ 以前 on_longpress() 写好了、但 timer **从没创建也没布防**，
    //    所以移动窗口的唯一入口只剩顶部 44*scale 那条细带，手机上基本捏不到，
    //    手指一拖就落在四个角的缩放带里 —— 表现就是“只能缩放、没法移动”。
    g_lp_timer = wl_event_loop_add_timer(g_srv.loop, on_longpress, nullptr);
    trace_init();

    wl_global_create(g_srv.dpy, &wl_compositor_interface, 6, nullptr, bind_compositor);
    wl_global_create(g_srv.dpy, &wl_subcompositor_interface, 1, nullptr, bind_subcompositor);
    wl_global_create(g_srv.dpy, &zwp_linux_dmabuf_v1_interface, 4, nullptr, bind_dmabuf);
    wl_global_create(g_srv.dpy, &wl_drm_interface, 2, nullptr, bind_drm);
    wl_global_create(g_srv.dpy, &wl_shm_interface, 1, nullptr, bind_shm);
    wl_global_create(g_srv.dpy, &wl_output_interface, 4, nullptr, bind_output);
    sc_screencopy_init(g_srv.dpy, g_out_w, g_out_h);   // zwlr_screencopy_manager_v1（见 sc_screencopy.hpp）
    // seat 版本：aquamarine（Hyprland 的 wayland 后端）是硬编码 bind(wl_seat, 9) 的，
    // 宣告 7 会被我们自己的 libwayland-server 拒绝：
    //   "invalid version for global wl_seat (7): expected at most 7, got 9"
    // 所以必须宣告 9。v8/v9 对我们没有新接口，只是版本号。
    wl_global_create(g_srv.dpy, &wl_seat_interface, 9, nullptr, bind_seat);
    wl_global_create(g_srv.dpy, &xdg_wm_base_interface, 6, nullptr, bind_wm_base);

    // 常见客户端/窗管都会 bind 的协议（存在即胜利，具体行为见前面的实现）
    wl_global_create(g_srv.dpy, &wp_viewporter_interface, 1, nullptr, bind_viewporter);
    wl_global_create(g_srv.dpy, &wp_fractional_scale_manager_v1_interface, 1, nullptr, bind_fracescale);
    wl_global_create(g_srv.dpy, &wp_cursor_shape_manager_v1_interface, 1, nullptr, bind_cursorshape);
    wl_global_create(g_srv.dpy, &wp_single_pixel_buffer_manager_v1_interface, 1, nullptr, bind_spbuffer);
    wl_global_create(g_srv.dpy, &wp_tearing_control_manager_v1_interface, 1, nullptr, bind_tearing);
    wl_global_create(g_srv.dpy, &zwp_relative_pointer_manager_v1_interface, 1, nullptr, bind_relptr);
    wl_global_create(g_srv.dpy, &zwp_pointer_constraints_v1_interface, 1, nullptr, bind_ptrcon);
    wl_global_create(g_srv.dpy, &zwp_idle_inhibit_manager_v1_interface, 1, nullptr, bind_idleinhibit);
    wl_global_create(g_srv.dpy, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1, nullptr, bind_ksinhibit);
    wl_global_create(g_srv.dpy, &zxdg_output_manager_v1_interface, 3, nullptr, bind_xdgoutput);
    wl_global_create(g_srv.dpy, &zxdg_decoration_manager_v1_interface, 1, nullptr, bind_decoration);
    wl_global_create(g_srv.dpy, &wl_data_device_manager_interface, 3, nullptr, bind_datadev);
    wl_global_create(g_srv.dpy, &zwp_primary_selection_device_manager_v1_interface, 1, nullptr, bind_primsel);
    wl_global_create(g_srv.dpy, &xwayland_shell_v1_interface, 1, nullptr, bind_xwlshell);
    wl_global_create(g_srv.dpy, &zwp_text_input_manager_v3_interface, 2, nullptr, bind_ti_mgr);
    // 面板/托盘/启动器（quickshell、waybar、fuzzel、swaybg 的地基）
    wl_global_create(g_srv.dpy, &zwlr_layer_shell_v1_interface, 4, nullptr, bind_lshell);
    wl_global_create(g_srv.dpy, &xdg_activation_v1_interface, 1, nullptr, bind_activation);
    wl_global_create(g_srv.dpy, &wp_presentation_interface, 2, nullptr, bind_presentation);
    // 小型扩展（客户端探测存在性）
    wl_global_create(g_srv.dpy, &wp_content_type_manager_v1_interface, 1, nullptr, bind_content_type);
    wl_global_create(g_srv.dpy, &wp_alpha_modifier_v1_interface, 1, nullptr, bind_alpha_modifier);
    wl_global_create(g_srv.dpy, &wp_fifo_manager_v1_interface, 1, nullptr, bind_fifo);
    wl_global_create(g_srv.dpy, &zwp_pointer_gestures_v1_interface, 3, nullptr, bind_pointer_gestures);
    wl_global_create(g_srv.dpy, &wp_pointer_warp_v1_interface, 1, nullptr, bind_pointer_warp);
    if (syncobj_supported())
        wl_global_create(g_srv.dpy, &wp_linux_drm_syncobj_manager_v1_interface, 1, nullptr, bind_syncobj);
    else
        printf("[syncobj] 驱动不支持 DRM_CAP_SYNCOBJ_TIMELINE，不宣告该协议（客户端自动退化为隐式同步）\n");

    if (wl_display_add_socket(g_srv.dpy, g_socket) != 0) {
        fprintf(stderr, "[sfwm] add_socket 失败: %s\n", strerror(errno));
        return 1;
    }
    const char *rt = getenv("XDG_RUNTIME_DIR");
    // 允许非 root 的 Termux 用户连进来（本进程通常是 root 起的）
    // 注意：目录本身由用户在会话脚本里 mkdir -p 700 建好（属于用户自己），这里不动它的权限，
    // 只把 socket 放开到 0666。
    if (rt) {
        mkdir(rt, 0700);
        std::string sock = std::string(rt) + "/" + g_socket;
        chmod(sock.c_str(), 0666);
    }
    printf("[sfwm] WAYLAND_DISPLAY=%s  (socket 在 %s/%s)\n",
           g_socket, rt ? rt : "?", g_socket);

    int evfd = open_touch_device(g_grab);
    if (evfd >= 0) {
        g_have_input = true;
        wl_event_loop_add_fd(g_srv.loop, evfd, WL_EVENT_READABLE, on_input_fd, nullptr);
    } else {
        printf("[input] 没找到触摸设备（读 /dev/input 需要 root）\n");
    }

    sfwm_keys_init();   // 键盘注入 socket（没有物理键盘也能打字）

    wl_display_run(g_srv.dpy);
    return 0;
}
