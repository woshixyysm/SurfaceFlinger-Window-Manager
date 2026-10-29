// SPDX-License-Identifier: AGPL-3.0-or-later
// output.hpp
//
// wl_output + xdg_output
//
// Extracted from the former single-file implementation; behaviour is unchanged.
// Depends on include order: everything used here is provided by the modules
// included before it in src/main.cpp.
#pragma once

// ---------------------------------------------------------------- wl_output
static void output_res_gone(wl_resource *r);
static void output_release(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct wl_output_interface output_impl = {
    .release = output_release,
};

// Send the full set of wl_output events, for binds and internal resources alike.
static void output_send_all(wl_resource *r, uint32_t version) {
    // The physical size in millimetres drives the DPI a client computes. A fixed
    // 340x750 mm made Qt report about 95 dpi, unreadable on this 475 ppi panel.
    // SFWM_DPI selects the logical DPI and the millimetre values follow from it.
    {
        int32_t dpi = 0;
        if (const char *e = getenv("SFWM_DPI")) dpi = atoi(e);
        if (dpi <= 0) dpi = 168;   // 168 by default; OBS still fits in 1272 logical pixels, SFWM_DPI overrides it
        if (dpi > 400) dpi = 400;
        int32_t mm_w = (int32_t)((int64_t)g_out_w * 254 / ((int64_t)dpi * 10));
        int32_t mm_h = (int32_t)((int64_t)g_out_h * 254 / ((int64_t)dpi * 10));
        if (mm_w < 1) mm_w = 1;
        if (mm_h < 1) mm_h = 1;
        printf("[out] reporting physical size %dx%d mm (dpi %d, output %dx%d)\n",
               mm_w, mm_h, dpi, g_out_w, g_out_h);
        wl_output_send_geometry(r, 0, 0, mm_w, mm_h, WL_OUTPUT_SUBPIXEL_UNKNOWN,
                                "sfwm", "SurfaceFlinger", WL_OUTPUT_TRANSFORM_NORMAL);
    }
    wl_output_send_mode(r, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                        g_out_w, g_out_h, 60000);
    if (version >= 2) wl_output_send_scale(r, g_scale);
    if (version >= 4) {
        wl_output_send_name(r, "SFWM-1");
        wl_output_send_description(r, "sfwm (SurfaceFlinger layer output)");
    }
    if (version >= 2) wl_output_send_done(r);
}

// Other modules need the wl_output resource of a given client. A temporary resource
// cannot be created: server-created objects only get an id when sent as a new_id
// argument, and wl_output has no new_id event, so the client rejects the result as an
// unknown object. Only resources the clients actually bound are kept here.
static std::vector<wl_resource *> g_out_ress;

static void output_res_gone(wl_resource *r) {
    for (size_t i = 0; i < g_out_ress.size(); i++)
        if (g_out_ress[i] == r) { g_out_ress.erase(g_out_ress.begin() + i); break; }
}
static wl_resource *output_for_client(wl_client *c) {
    if (!c) return nullptr;
    for (wl_resource *r : g_out_ress)
        if (wl_resource_get_client(r) == c) return r;
    return nullptr;
}

static void bind_output(wl_client *c, void *, uint32_t version, uint32_t id) {
    wl_resource *r = wl_resource_create(c, &wl_output_interface, version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &output_impl, nullptr, output_res_gone);
    g_out_ress.push_back(r);
    output_send_all(r, version);
}

