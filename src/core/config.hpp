// config.hpp
//
// 配置全局 + 缓冲/连接/Server 结构体
//
// 从 sfwm.cpp 原样搬出（只搬不改，逻辑零差异）。
// 依赖包含顺序：本文件用到的东西必须由**前面的**模块提供，
// 因此它在 sfwm.cpp 里的 include 位置就是它原来在文件里的位置。
#pragma once

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

