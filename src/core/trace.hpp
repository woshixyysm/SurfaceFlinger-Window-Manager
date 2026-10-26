// trace.hpp
//
// 结构化跟踪（SFWM_TRACE）
//
// 从 sfwm.cpp 原样搬出（只搬不改，逻辑零差异）。
// 依赖包含顺序：本文件用到的东西必须由**前面的**模块提供，
// 因此它在 sfwm.cpp 里的 include 位置就是它原来在文件里的位置。
#pragma once

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

