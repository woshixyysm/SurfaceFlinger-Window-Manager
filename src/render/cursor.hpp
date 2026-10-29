// SPDX-License-Identifier: AGPL-3.0-or-later
// cursor.hpp
//
// Cursor geometry, Xcursor theme parsing and presentation.
//
// Extracted from the former single-file implementation; behaviour is unchanged.
// Depends on include order: everything used here is provided by the modules
// included before it in src/main.cpp.
#pragma once

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

// scanline even-odd fill, with points flattened as x0,y0,x1,y1
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

// black outline and white fill: shrink the vertices towards the centroid first
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

// ---- theme loading, parsing Xcursor files directly
// libwayland-cursor is not used: its wl_cursor_image has no data member here and it
// needs a client side wl_shm, which a server does not have.
static const char *cursor_shape_name(uint32_t s);

// cursor-shape-v1 enumeration to Xcursor file name
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

// the Xcursor hashed file name scheme, used by the hex named files in themes
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

// read an Xcursor file, picking the size closest to want
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
        if (type != 0xfffd0002u) continue;              // IMAGE chunks only
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

// look for theme/cursors/name in XCURSOR_PATH, plain first, then hashed
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
    // The cursor is drawn in physical pixels: 24 px is too small on a 1272x2800 output.
    // Anything below 2x is treated as 2x, and XCURSOR_SIZE overrides it.
    const int32_t eff = (g_scale > 1 ? g_scale : 2);
    const int32_t want = base * eff;

    // sfwm is started through su, which drops XCURSOR_* and may leave HOME empty, so the
    // fixed Termux paths are hard coded as a fallback, otherwise the theme in
    // ~/.icons/Bibata-Modern-Classic is never found.
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
                        printf("[cursor] hit %s/%s/cursors/%s\n", dir, themes[ti], tryn[k]);
                        return true;
                    }
                }
                p = colon ? colon + 1 : nullptr;
            }
        }
    }
    printf("[cursor] no path matched (shape=%u want=%d) paths=%s\n", shape, want, paths);
    return false;
}

// bidirectional arrow: dir 0 horizontal, 1 vertical, 2 slash, 3 backslash
static void cur_dbl_arrow(int dir, int32_t thick, uint32_t c) {
    const int32_t C = CUR_N / 2;
    const int32_t lo = 2, hi = CUR_N - 3;           // the two ends
    const int32_t ah = 5;                            // half height of the arrow head
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

    // Prefer a real X cursor theme when one is configured.
    if (xc_load(shape, &g_cur_w, &g_cur_h, &g_cur_hx, &g_cur_hy, &g_cur_px)) {
        g_cur_mul = 1;
        g_cur_built = true;
        printf("[cursor] theme image %dx%d hot=(%d,%d) shape=%u\n",
               g_cur_w, g_cur_h, g_cur_hx, g_cur_hy, shape);
        return;
    }
    printf("[cursor] shape=%u not in the theme, using the built-in arrow\n", shape);

    // Fallback: a procedural 24x24 arrow, so there is always a cursor.
    g_cur_w = g_cur_h = CUR_N;
    g_cur_px = (uint32_t *)calloc((size_t)CUR_N * CUR_N, 4);
    if (!g_cur_px) { g_cur_w = g_cur_h = 0; return; }
    for (int i = 0; i < CUR_N * CUR_N; i++) g_cur_px[i] = CUR_CLR;
    g_cur_mul = 0;      // 0 means scale by g_scale when drawing

    switch (shape) {
    case 9: case 10: {                     // text and vertical-text: I beam
        cur_rect(6, 2, 12, 4, CUR_OPQ);
        cur_rect(8, 2, 9, 20, CUR_OPQ);
        cur_rect(6, 18, 12, 20, CUR_OPQ);
        cur_rect(7, 3, 11, 3, CUR_WHT);
        cur_rect(9, 4, 9, 18, CUR_WHT);
        cur_rect(7, 19, 11, 19, CUR_WHT);
        g_cur_hx = 9; g_cur_hy = 11;
        break;
    }
    case 4: case 16: case 17: {            // pointer, grab and grabbing: hand
        cur_disc(11, 15, 6, CUR_OPQ);
        cur_rect(4, 12, 17, 20, CUR_OPQ);
        cur_rect(9, 3, 12, 15, CUR_OPQ);   // index finger
        cur_disc(11, 15, 4, CUR_WHT);
        cur_rect(6, 13, 15, 19, CUR_WHT);
        cur_rect(10, 4, 11, 14, CUR_WHT);
        g_cur_hx = 10; g_cur_hy = 4;
        break;
    }
    case 7: case 8: {                      // cell and crosshair: cross
        cur_rect(2, 10, CUR_N - 3, 12, CUR_OPQ);
        cur_rect(10, 2, 12, CUR_N - 3, CUR_OPQ);
        cur_rect(3, 11, CUR_N - 4, 11, CUR_WHT);
        cur_rect(11, 3, 11, CUR_N - 4, CUR_WHT);
        g_cur_hx = 11; g_cur_hy = 11;
        break;
    }
    case 13: case 32: {                    // move and all-scroll: four arrows
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
    case 20: case 24: case 28: {           // ne / sw / nesw-resize  ->  "/"
        cur_dbl_arrow(2, 2, CUR_OPQ);
        cur_dbl_arrow(2, 1, CUR_WHT);
        g_cur_hx = 11; g_cur_hy = 11;
        break;
    }
    case 21: case 23: case 29: {           // nw / se / nwse-resize  ->  "\"
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
    default: {                             // default arrow
        cur_poly_ol((int32_t[]){0, 0, 14, 13, 8, 13, 11, 20, 8, 21, 5, 14, 0, 19}, 7);
        g_cur_hx = 0; g_cur_hy = 0;
        break;
    }
    }
    g_cur_built = true;
}

// place the cursor at g_cur_x, g_cur_y in physical pixels
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
        // The touchable region is already empty, but Android 12+ still swallows touches
        // under the cursor through touchOcclusionMode, so the layer has to be declared
        // trusted and to drop all input.
        if (!g_cur_layer.makeInputTransparent())
            printf("[cursor] warning: makeInputTransparent failed (no setTrustedOverlay or setDropInputMode)\n");
        ANativeWindow_setBuffersGeometry(g_cur_layer.win, pw, ph, WINDOW_FORMAT_RGBA_8888);
        g_cur_layer.setGeometry(sf::Rect{0, 0, pw, ph}, sf::Rect{0, 0, pw, ph});
        g_cur_lw = pw; g_cur_lh = ph;
    }
    // This has to be set every time: sf::Layer::create() resets m_touchable, and a
    // cursor layer rebuilt at a new size would eat clicks again.
    
    g_cur_layer.setTouchable(false);
    // Show and apply first, or ANativeWindow_lock fails; same order as in blit().
    g_cur_layer.place(g_cur_x - g_cur_hx * mul, g_cur_y - g_cur_hy * mul, g_base_z + 60000);

    ANativeWindow_Buffer ab;
    if (ANativeWindow_lock(g_cur_layer.win, &ab, nullptr) != 0) return;
    if (ab.bits && ab.stride > 0) {
        for (int32_t ly = 0; ly < g_cur_h; ly++) {
            for (int32_t lx = 0; lx < g_cur_w; lx++) {
                uint32_t c = g_cur_px[ly * g_cur_w + lx];
                // ARGB8888 to RGBA8888, that is memory order B,G,R,A on little endian
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

