// testpat.hpp
//
// 自检图案（SFWM_TESTPAT，逐项验证不闪）
//
// 从 sfwm.cpp 原样搬出（只搬不改，逻辑零差异）。
// 依赖包含顺序：本文件用到的东西必须由**前面的**模块提供，
// 因此它在 sfwm.cpp 里的 include 位置就是它原来在文件里的位置。
#pragma once

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

