// presentation.hpp
//
// wp_presentation_feedback
//
// 从 sfwm.cpp 原样搬出（只搬不改，逻辑零差异）。
// 依赖包含顺序：本文件用到的东西必须由**前面的**模块提供，
// 因此它在 sfwm.cpp 里的 include 位置就是它原来在文件里的位置。
#pragma once

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

