// output.hpp
//
// wl_output + xdg_output
//
// 从 sfwm.cpp 原样搬出（只搬不改，逻辑零差异）。
// 依赖包含顺序：本文件用到的东西必须由**前面的**模块提供，
// 因此它在 sfwm.cpp 里的 include 位置就是它原来在文件里的位置。
#pragma once

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

