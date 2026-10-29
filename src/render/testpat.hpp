// SPDX-License-Identifier: AGPL-3.0-or-later
// testpat.hpp
//
// Self-test pattern (SFWM_TESTPAT), used to isolate flicker.
//
// Extracted from the former single-file implementation; behaviour is unchanged.
// Depends on include order: everything used here is provided by the modules
// included before it in src/main.cpp.
#pragma once

// ---------------------------------------------------------------- View

// Composite test pattern, used to tell whether flicker comes from the upload path or from
// the client content. With SFWM_TESTPAT=1 a static pattern is drawn over the layer buffer:
// four blocks at the top that never change, a gradient line that shows tearing, and a
// ticking block in the bottom right that changes colour on every post.
static int testpat_mode() {
    static int m = -2;
    if (m == -2) { const char *e = getenv("SFWM_TESTPAT"); m = e ? atoi(e) : 0; }
    return m;
}
static uint32_t g_tp_tick = 0;

// The layer is BGRA_8888, so a little endian uint32 is A<<24 R<<16 G<<8 B.
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

