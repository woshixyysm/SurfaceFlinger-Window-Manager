// SPDX-License-Identifier: AGPL-3.0-or-later
// config.hpp
//
// Configuration globals and the buffer, connection and server structures.
//
// Extracted from the former single-file implementation; behaviour is unchanged.
// Depends on include order: everything used here is provided by the modules
// included before it in src/main.cpp.
#pragma once

// ------------------------------------------------------------- configuration
static int32_t g_out_w = 1272, g_out_h = 2800;
static int32_t g_base_z = 1000;
// Click-through: our layers stop receiving touches, which reach the Android UI below.
static bool g_clickthrough = false;
// Set while dragging, to detect whether the client keeps redrawing.
static bool g_in_drag = false;
// Snapping to the screen edges and centre line, toggled by the injector at runtime.
static bool g_snap_on = true;
static int32_t g_snap_band_dp = 24;
static int g_snap_bits = 0;      // 1 left, 2 centre, 4 right, 8 top
static const char *g_socket = "sfwm-0";
static bool g_grab = false;
static int32_t g_scale = 3;   // the default desktop scale is too small on a 560 dpi panel

// ---- Android soft keyboard, hosted by the input method service
static int32_t g_ime_h = 0;    // keyboard height in screen pixels, 0 when hidden
static bool    g_ime_on = false;
// The persistent connection to the input method client. Activate and deactivate are
// written to it directly, replacing polling of settings, am and a state file.
static int     g_ime_fd = -1;
// Sequence of the background band: full-screen windows stay below content windows.
static int32_t g_bg_seq = 0;
// Whether the session is locked. While locked only the lock surface is visible; the
// flag is also read by ws_visible() to block View::blit().
static bool g_lock_active = false;

static void ime_send(bool show);          // forward declaration used by the text input code
static void ti_force_disable();           // likewise

// ---- wl_shm buffers, since the wayland ones carry no offset
static const uint32_t SHMBUF_MAGIC = 0x53464246u;  // 'SBFB'

// A shm pool may be destroyed before its buffers, so it is reference counted and
// unmapped when the last user releases it.
struct ShmPool {
    int fd = -1;
    int32_t size = 0;
    uint8_t *base = nullptr;
    int refs = 1;          // one for the resource, one per live buffer
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
    ShmPool *pool = nullptr;   // holds a pool reference, so base is not unmapped early
    int32_t offset = 0;
    int32_t w = 0, h = 0, stride = 0;
    uint32_t format = 0;
};

// ---- dmabuf buffers (zwp_linux_dmabuf_v1)
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

// One connection of the injection socket
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
    // input
    std::vector<wl_resource *> pointers;
    std::vector<wl_resource *> touches;
    std::vector<wl_resource *> keyboards;
    std::vector<wl_resource *> text_inputs;   // zwp_text_input_v3
    View *pointer_focus = nullptr;
    View *kb_focus = nullptr;       // keyboard focus, given to the window clicked at
    View *cursor_view = nullptr;
    int32_t cursor_hot_x = 0, cursor_hot_y = 0;
    int32_t pointer_x = 0, pointer_y = 0;   // last pointer position in screen coordinates
    View *move_view = nullptr;      // interactive move started by xdg_toplevel.move
    // keyboard injection socket
    int key_listen_fd = -1;
    std::string key_sock_path;
    std::vector<KeyConn *> key_conns;
};

static Server g_srv;

