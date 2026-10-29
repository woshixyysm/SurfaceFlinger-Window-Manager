// SPDX-License-Identifier: AGPL-3.0-or-later
// trace.hpp
//
// Structured tracing (SFWM_TRACE).
//
// Extracted from the former single-file implementation; behaviour is unchanged.
// Depends on include order: everything used here is provided by the modules
// included before it in src/main.cpp.
#pragma once

// ------------------------------------------------------------- structured tracing
// With SFWM_TRACE=1 geometry changes, hit tests and gesture state are written to a file
// for offline analysis. Every record carries a reason, so the step that moved a window
// can be identified.
static FILE   *g_trace_fp = nullptr;
static bool    g_trace_on = false;
static const char *g_reason = "?";
// A gesture is in progress, so geometry updates are rate limited to avoid flicker.
static bool g_in_gesture = false;
static int  touch_owner_value(void);   // defined after the touch state machine declares g_owner

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
    if (g_trace_on) printf("[trace] writing to /data/local/tmp/sfwm-trace.log\n");
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

// Whether a client draws its own cursor. Adding the built-in cursor on top would show
// two arrows.
static bool g_have_client_cursor = false;

// The view the button was pressed on. The release goes there even when the focus was
// cleared in the meantime, otherwise the client believes the button is still down and
// treats every later click as a drag.
static View *g_btn_view = nullptr;

// Delayed confirmation of a corner drag. Pressing inside a corner band used to start a
// resize immediately, which made buttons near the bottom edge unreachable. The corner is
// only armed here; the resize starts once the finger actually moves.
static int32_t g_arm_corner = 0;
static int32_t g_arm_x = 0, g_arm_y = 0;

// Forward declaration used by View::map() and blit().
static void kb_set_focus(View *v);
static void toplevel_send_configure(View *v);
static int32_t client_scale(View *v);
static void layer_apply_anchor(View *v);
static void pres_send_presented(View *v);
static void sync_wait_acquire(View *v);
static void sync_signal_release(View *v);

// -----------------------------------------------------------------------------
// ------------------------------------------------------------- built-in cursor
//
// The cursor is drawn here because wl_pointer.set_cursor is only sent by toolkits that
// manage their own cursor, and wp_cursor_shape_v1 supplies a shape number without pixels.
// The arrow is rasterised at 24x24 logical pixels (white fill, black outline) onto its
// own SurfaceFlinger layer and redrawn whenever the pointer moves.
static const int32_t CUR_N = 24;                 // logical size of the fallback arrow
static sf::Layer     g_cur_layer;
static uint32_t     *g_cur_px = nullptr;         // ARGB8888 bitmap
static int32_t       g_cur_w = 0, g_cur_h = 0;
static int32_t       g_cur_hx = 0, g_cur_hy = 0; // hotspot in bitmap coordinates
static int32_t       g_cur_mul = 1;              // magnification, 0 means follow g_scale
static int32_t       g_cur_lw = 0, g_cur_lh = 0; // current physical layer size
static uint32_t      g_cur_shape = 1;            // wp_cursor_shape_v1, 1 is default
static bool          g_cur_built = false;
static bool          g_cur_shown = false;
static int32_t       g_cur_x = 0, g_cur_y = 0;   // last pointer position in physical pixels

#define CUR_OPQ 0xFF000000u   // opaque black, used for the outline
#define CUR_WHT 0xFFFFFFFFu   // white, used for the fill
#define CUR_CLR 0x00000000u   // fully transparent

