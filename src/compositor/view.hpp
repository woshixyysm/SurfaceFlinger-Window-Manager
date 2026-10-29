// SPDX-License-Identifier: AGPL-3.0-or-later
// view.hpp - per-surface state, geometry and stacking helpers.
//
// Requires include order: sf_layer.hpp, wayland-server, <vector>, the Server
// forward declarations and the globals declared above it in src/main.cpp must
// be visible before this header is included.
#pragma once

struct View;
static void ftl_on_view_mapped(View *v);    // ext-foreign-toplevel-list
static void ftl_on_view_changed(View *v);   // title or app_id changed
static void ftlm_on_view_mapped(View *v);   // zwlr-foreign-toplevel-management
static void ftlm_on_view_changed(View *v);
static void ws_assign_new(View *v);         // ext-workspace: join the active workspace
static bool ws_visible(View *v);            // ext-workspace: visible in the active workspace
static void lock_on_mapped(View *v);        // ext-session-lock: first map of the lock surface

// -------------------------------------------------------- wl_surface.frame throttling
//
// A frame callback does not mean "shown on screen"; it means "you may draw the next
// frame". Sending wl_callback.done straight from commit() gives clients no rate limit,
// so GPU clients redraw as fast as they can and recycle dmabufs while the compositor is
// still sampling them. Defer the callbacks and release them from a 16 ms timer instead.
//     "kitty already uses the wayland render frame protocol to throttle rendering"
struct FrameSlot { wl_listener lis; wl_resource *cb; };
static std::vector<wl_resource *> g_frame_pending;
static wl_event_source *g_frame_timer = nullptr;
static bool g_frame_armed = false;   // one-shot timer: re-armed on every tick

static void frame_cb_gone(wl_listener *l, void *) {
    FrameSlot *s = wl_container_of(l, s, lis);
    for (size_t i = 0; i < g_frame_pending.size(); i++)
        if (g_frame_pending[i] == s->cb) { g_frame_pending.erase(g_frame_pending.begin() + i); break; }
    wl_list_remove(&s->lis.link);
    delete s;
}

// Declared after struct View, which holds the callback it consumes.
static void frame_defer(View *v);

// Every ~16 ms: release the queued frame callbacks and re-arm the timer. The timer is
// one-shot, so a tick that forgets to re-arm stops the clock and strands the queue.
static int on_frame_tick(void *) {
    if (!g_frame_pending.empty()) {
        std::vector<wl_resource *> batch;
        batch.swap(g_frame_pending);
        const uint32_t t = now_ms();
        for (wl_resource *cb : batch) {
            wl_callback_send_done(cb, t);
            wl_resource_destroy(cb);
        }
    }
    if (!g_frame_pending.empty()) {          // callbacks arrived this tick: keep the cadence
        if (g_frame_timer) wl_event_source_timer_update(g_frame_timer, 16);
    } else {
        g_frame_armed = false;               // idle: arm again on the next callback
    }
    return 0;
}

// -------------------------------------------------------- dmabuf implicit fence
//
// A dmabuf is usually submitted before the GPU has finished writing it. DMA_BUF_IOCTL_SYNC
// only flushes CPU caches and does not wait for the fence. Android kernels export the
// implicit fence through DMA_BUF_IOCTL_EXPORT_SYNC_FILE; poll() on that fd reports the
// write as done. Kernels without it return -ENOTTY and we fall back to blind reads.
#ifndef DMA_BUF_IOCTL_EXPORT_SYNC_FILE
struct dma_buf_export_sync_file { uint32_t flags; int32_t fd; };
#  define DMA_BUF_IOCTL_EXPORT_SYNC_FILE \
       _IOWR('b', 2, struct dma_buf_export_sync_file)
#endif
#include <poll.h>
#include <sys/ioctl.h>

static bool g_fence_ok = false;
static bool g_fence_checked = false;

// true if a fence was waited on; false if there was none or the wait timed out
static bool dbuf_wait_fence(int fd, int timeout_ms) {
    struct dma_buf_export_sync_file arg;
    memset(&arg, 0, sizeof(arg));
    arg.flags = DMA_BUF_SYNC_READ;
    arg.fd = -1;
    if (ioctl(fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &arg) < 0) {
        if (!g_fence_checked) {
            g_fence_checked = true;
            printf("[sync] kernel has no DMA_BUF_IOCTL_EXPORT_SYNC_FILE (%s)\n"
                   "       cannot wait for the GPU fence, reading without synchronisation\n", strerror(errno));
        }
        return false;
    }
    if (!g_fence_checked) {
        g_fence_checked = true;
        g_fence_ok = true;
        printf("[sync] dma-buf implicit fence available, waiting for the GPU before reading\n");
    }
    if (arg.fd < 0) return false;      // no pending fence: the write has completed
    struct pollfd p;
    p.fd = arg.fd;
    p.events = POLLIN;
    p.revents = 0;
    const int r = poll(&p, 1, timeout_ms);
    close(arg.fd);
    return r > 0;
}

struct View {
    Server *srv = nullptr;
    wl_resource *surface = nullptr;
    wl_resource *xdg_surface = nullptr;
    wl_resource *toplevel = nullptr;
    wl_resource *pending_buffer = nullptr;
    // Read one frame late: the buffer is consumed when the next one is committed, by
    // which time the GPU work for it has certainly finished.
    wl_resource *read_next = nullptr;
    uint32_t read_next_ms = 0;   // time the pending frame was taken (watchdog)
    // read_next outlives a single commit. Clients may destroy the wl_buffer while
    // resizing or rebuilding a swapchain, so a destroy listener clears the pointer.
    struct HoldListener { wl_listener lis; View *view; };
    HoldListener *hold = nullptr;
    wl_resource *frame_cb = nullptr;
    sf::Layer layer;
    int32_t bw = 0, bh = 0;            // current buffer size
    int32_t req_w = 0, req_h = 0;      // logical size requested while dragging (0 = none)
    int32_t x = 0, y = 0;
    int32_t z = 0;
    // Composer-side zoom during a gesture: changes only the layer destination geometry,
    // never the client buffer, so no configure is sent and the client cannot flicker.
    float zoom = 1.f;
    bool mapped = false;
    bool ftl_notified = false;      // toplevel list already told about this window
    int  ws = 0;                    // workspace slot (ext_workspace_manager_v1)
    int  geom_buf_w = 0, geom_buf_h = 0;   // size used by the last setBuffersGeometry call
    bool is_lock = false;           // session-lock surface (ext_session_lock_v1)
    // wp_fractional_scale_v1 preferred scale, rounded. Clients using the protocol render
    // at Nx but keep buffer_scale at 1, so the factor only arrives through this event.
    int  frac_scale = 0;
    // buffer pixels per logical pixel of this surface
    int sscale() const { return frac_scale > buf_scale ? frac_scale : buf_scale; }
    bool is_cursor = false;
    bool is_popup = false;
    bool is_ime = false;        // input method: always on top, hidden when inactive

    // ---- zwlr_layer_shell_v1 (panels, trays, launchers)
    bool is_layer = false;
    wl_resource *layer_surface = nullptr;
    uint32_t layer_anchor = 0;
    uint32_t layer_kb = 0;          // keyboard_interactivity
    uint32_t layer_num = 3;         // 1 = background, 2 = bottom, 3 = top, 4 = overlay
    int32_t layer_w = 0, layer_h = 0;      // logical size requested by the client (0 = unset)
    int32_t layer_excl = 0;
    int32_t layer_mt = 0, layer_mr = 0, layer_mb = 0, layer_ml = 0;
    char layer_ns[64] = {0};

    // ---- pending wp_presentation_feedback objects
    std::vector<wl_resource *> feedbacks;

    // ---- misc extensions
    uint32_t content_type = 0;      // wp_content_type_v1
    int32_t  alpha_mult   = 256;    // wp_alpha_modifier_v1, fixed 1.0

    // ---- last geometry reported to the trace
    int32_t last_x = -0x7fffffff, last_y = -0x7fffffff, last_z = -0x7fffffff;

    // ---- wp_linux_drm_syncobj_v1 (explicit sync)
    wl_resource *syncobj_surface = nullptr;
    uint32_t sync_acq_handle = 0, sync_rel_handle = 0;
    uint64_t sync_acq_point  = 0, sync_rel_point  = 0;
    bool     sync_has_acq = false, sync_has_rel = false;
    // The Xwayland root window has no content worth drawing: no layer, no stacking,
    // and it is excluded from input-method reservation.
    bool is_x_root = false;
    bool want_maximized = false;
    bool want_fullscreen = false;
    View *parent = nullptr;
    int32_t buf_scale = 1;      // wl_surface.set_buffer_scale; surface = physical / buf_scale
    bool    scale_calibrated = false;   // calibrate once so transient sizes are not read as scale
    bool    fit_requested    = false;   // send "please shrink" once, to avoid a configure loop
    int frames = 0;
    char title[128] = {0};
    char app_id[128] = {0};

    // ---- wl_subcompositor
    bool is_subsurface = false;
    View *sub_parent = nullptr;
    int32_t sub_x = 0, sub_y = 0;      // offset from the parent (surface-local coordinates)
    std::vector<View *> subs;          // child surfaces

    // Stacking key: parent z*8, plus one slot for a subsurface above its parent.
    int32_t stack_key() const {
        if (is_subsurface && sub_parent) return sub_parent->z * 8 + 4;
        return z * 8;
    }

    // A window may only be visible above the keyboard when the input method is up.
    // Shifting a window taller than the screen does not help; the height has to be cropped.
    int32_t vis_h_for(int32_t sh) const {
    // Reserving space for the keyboard is off by default: the reported IME height may be
    // stale, and a wrong decision crops the bottom of every window permanently. Set
    // SFWM_IME_RESERVE=1 to restore the old behaviour.
        static int reserve = -1;
        if (reserve < 0) reserve = getenv("SFWM_IME_RESERVE") ? 1 : 0;
        if (!reserve || !g_ime_on || g_ime_h <= 0) return sh;
        int32_t avail = g_out_h - g_ime_h - y;
        if (avail < 60) avail = 60;
        return sh > avail ? avail : sh;
    }

    void place() {
        /* is_x_root: place normally (rootful X root window IS the content) */
        if (is_subsurface && sub_parent) {
    // Subsurface offsets must be scaled together with the parent. Toolkits that draw
    // their content in a subsurface would otherwise keep the content at the old size.
            const float pz = sub_parent->zoom > 0.f ? sub_parent->zoom : 1.f;
            const int pscale = sub_parent->sscale();
            x = sub_parent->x + (int32_t)((float)sub_x * pscale * pz);
            y = sub_parent->y + (int32_t)((float)sub_y * pscale * pz);
            z = sub_parent->z;
        }
    // The keyboard occupies the bottom g_ime_h pixels: crop the layer height rather than
    // shifting the window, which cannot work for windows taller than the screen.
        const int32_t shift = 0;
    // The input method sits above everything.
        const int32_t zz = g_base_z + (is_ime ? 5000 : stack_key());
        layer.place(x, y - shift, zz);
    // Windows are opaque: skip alpha blending and hide unstable client alpha.
        if (!is_cursor && !is_layer && !is_subsurface) layer.setOpaque(true);
        layer.setTouchable(!g_clickthrough);
        if (x != last_x || y != last_y || zz != last_z) {
            trace("W place '%s' %p (%d,%d)->(%d,%d) z=%d buf=%dx%d scale=%d reason=%s",
                  title[0] ? title : (app_id[0] ? app_id : "view"), (void *)this,
                  last_x, last_y, x, y, g_base_z + stack_key(), bw, bh, buf_scale, g_reason);
            last_x = x;
            last_y = y;
            last_z = g_base_z + stack_key();
        }
        g_reason = "?";   // not sticky: names only the step that produced this placement
        for (View *c : subs) if (c->mapped) c->place();
    }

    // Apply gesture zoom to the layer geometry: src is the full buffer, dst is the buffer
    // size times the zoom. The keyboard crop has to shorten both src and dst, otherwise
    // SurfaceFlinger scales the picture instead of cropping it.
    void apply_geometry_ex(int32_t src_w, int32_t src_h) {
        if (!layer.win || src_w <= 0 || src_h <= 0) return;
    // A subsurface always follows its parent zoom. Keeping a private copy left stale
    // factors behind after a gesture and smeared the borders across the window.
        const float z = (is_subsurface && sub_parent)
                            ? (sub_parent->zoom > 0.f ? sub_parent->zoom : 1.f)
                            : (zoom > 0.f ? zoom : 1.f);
        int32_t dw = (int32_t)(src_w * z + 0.5f);
        int32_t dh = (int32_t)(src_h * z + 0.5f);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        int32_t vh = vis_h_for(dh);          // pixels left above the keyboard
        if (vh < 1) vh = 1;
        int32_t src_hh = src_h;
        if (vh < dh) {
            src_hh = (int32_t)((int64_t)src_h * vh / dh);
            if (src_hh < 1) src_hh = 1;
        }
        layer.setGeometry(sf::Rect{0, 0, src_w, src_hh},
                          sf::Rect{0, 0, dw, vh});
        if (getenv("SFWM_GEOBG"))
            printf("[geo] '%s' zoom=%.2f src=%dx%d src_hh=%d dst=%dx%d vh=%d ime_h=%d pos=(%d,%d)\n",
                   title[0] ? title : "view", z, src_w, src_h, src_hh, dw, vh, vh,
                   g_ime_h, x, y);
    }

    void apply_zoom() { apply_geometry_ex(bw, bh); }

    // Scale recursively: when the parent scales, the child surface holding the client
    // content has to scale with it, or the content stays at the old size and drifts.
    void set_zoom(float z) {
    // Accumulate the whole change in one SurfaceFlinger transaction and apply it once.
    // Applying per layer exposes intermediate states to the display and flickers.
        sf::begin_batch();
        zoom = z;
        apply_zoom();
        for (View *c : subs) {
            if (!c->mapped) continue;
            c->zoom = z;
            c->apply_zoom();
        }
        place();          // recurses into child surfaces, in the same transaction
        sf::end_batch();
    }

    // Effective size after zoom (matches the layer dst size and is used for hit testing).
    int32_t ew() const { return (int32_t)(bw * (zoom > 0.f ? zoom : 1.f) + 0.5f); }
    int32_t eh() const { return (int32_t)(bh * (zoom > 0.f ? zoom : 1.f) + 0.5f); }
    // Screen coordinates to surface-local coordinates. During a gesture the divisor has
    // to include the zoom, otherwise input misses the pixels it appears to hit.
    int32_t to_local_x(int32_t sx) const {
        const float s = (float)sscale() * (zoom > 0.f ? zoom : 1.f);
        return (int32_t)((sx - x) / s);
    }
    int32_t to_local_y(int32_t sy) const {
        const float s = (float)sscale() * (zoom > 0.f ? zoom : 1.f);
        return (int32_t)((sy - y) / s);
    }

    // The toplevel lists are notified from here, once, because map() is dead code: the
    // mapping is set directly in the shm and dmabuf paths of blit().
    void notify_mapped() {
        if (ftl_notified) return;
        ftl_notified = true;
        ws_assign_new(this);         // new windows join the active workspace
        lock_on_mapped(this);        // a lock surface enters the locked state here
        ftl_on_view_mapped(this);    // ext-foreign-toplevel-list
        ftlm_on_view_mapped(this);   // zwlr-foreign-toplevel-management
    }

    void map() {
        if (mapped) return;
        g_reason = "map";
        mapped = true;
        notify_mapped();
        if (!is_x_root && strstr(title, "Xwayland on ") != nullptr) is_x_root = true;
        // Input method: app_id or title containing sfwm.ime means our own keyboard
        if (!is_ime && (strstr(app_id, "sfwm.ime") || strstr(title, "sfwm-ime") ||
                        strstr(app_id, "org.sfwm.ime"))) {
            is_ime = true;
            printf("[wm] '%s' detected as input method -> pinned on top\n", title[0] ? title : app_id);
        }
        if (is_x_root)
            printf("[wm] X11 root window -> background layer\n");
        if (is_layer) layer_apply_anchor(this);
        place();
        if (!is_subsurface && !is_cursor) clamp_on_screen();
        // Default stacking: a window that covers the whole output is treated as a
        // background and kept below the content windows; everything else stacks on top
        // when mapped. Without this a full-screen black window would cover the shell.
        if (!is_subsurface && !is_cursor && !is_layer) {
            if (bw * 100 >= g_out_w * 95 && bh * 100 >= g_out_h * 95) {
                z = -1000 + (g_bg_seq++);
                place();
                printf("[wm] '%s' fullscreen %dx%d -> background z=%d\n",
                       title[0] ? title : (app_id[0] ? app_id : "view"), bw, bh, z);
            } else {
                int32_t top = -1;
                for (View *o : g_srv.views)
                    if (o->mapped && !o->is_cursor && !o->parent && !o->is_layer && o != this && o->z > top) top = o->z;
                if (z <= top) {
                    z = top + 1;
                    place();
                    printf("[wm] '%s' mapped on top z=%d\n",
                           title[0] ? title : (app_id[0] ? app_id : "view"), z);
                }
            }
        }
        printf("[sfwm] map '%s' %dx%d @ (%d,%d) z=%d\n",
               title[0] ? title : (app_id[0] ? app_id : "?"), bw, bh, x, y, g_base_z + z);
        if (!is_subsurface && !is_cursor && !is_x_root && !is_layer) kb_set_focus(this);   // new window takes focus
        // Layer surfaces take focus only when they declare keyboard interactivity.
        if (is_layer && layer_kb != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) kb_set_focus(this);
    }

    // Fit a newly mapped window into the output:
    //   1) clamp its position, so it is not cropped at the right edge;
    //   2) if it is larger than the output, send one configure asking for a smaller size.
    void fit() {
        if (is_subsurface || is_cursor || is_layer) return;
        g_reason = "fit";
        // Ask for a smaller size once and never move the window. Repeated requests cannot
        // win against a client minimum size and only make the window jump around.
        if (!fit_requested && (bw > g_out_w || bh > g_out_h)) {
            fit_requested = true;
            const int32_t sc = client_scale(this);
            int32_t lw = g_out_w / sc, lh = g_out_h / sc;
            if (lw < 320) lw = 320;
            if (lh < 240) lh = 240;
            req_w = lw;
            req_h = lh;
            toplevel_send_configure(this);
            printf("[wm] '%s' %dx%d larger than the output -> requesting %dx%d logical once\n",
                   title[0] ? title : "view", bw, bh, lw, lh);
        }
    }

    // pull a window back onto the screen. When it fits, keep it fully visible: dialogs
    // keep their buttons in the bottom right corner. Only a window larger than the output
    // degrades to "keep at least 80dp visible".
    void clamp_on_screen() {
        // Clamp with the effective size, not the unscaled buffer size: during a gesture the
        // buffer equals the output width and clamping with it forces x to zero.
        const int32_t W = ew(), H = eh();
        if (is_cursor || is_subsurface || is_layer) return;
        if (W <= 0 || H <= 0) return;
        const int32_t keep = 80 * (g_scale > 0 ? g_scale : 1);
        if (W <= g_out_w) {
            if (x < 0) x = 0;
            if (x + W > g_out_w) x = g_out_w - W;
        } else {
            if (x > g_out_w - keep) x = g_out_w - keep;
            if (x + W < keep) x = keep - W;
        }
        if (H <= g_out_h) {
            if (y < 0) y = 0;
            if (y + H > g_out_h) y = g_out_h - H;
        } else {
            if (y > g_out_h - keep) y = g_out_h - keep;
            if (y + H < keep) y = keep - H;
        }
        if (y < 0) y = 0;
        place();
    }

    // mmap cache for dmabufs: a client submits the same buffers repeatedly, and mapping
    // them on every frame is expensive. Six slots cover the usual 2-3 buffer swapchains.
    struct DMap { int fd = -1; size_t len = 0; uint8_t *p = nullptr; ino_t ino = 0; };

    static uint8_t *dmap_get(int fd, size_t len) {
        static DMap g_dmaps[6];   // function-local static, so no out-of-class definition
        // The cache is keyed by inode, not by fd. Swapchain rebuilds close the old fd and
        // the number is reused, so an fd-keyed hit would read a released buffer. Every
        // dma-buf has its own anon_inode inode, which fstat distinguishes.
        struct stat st;
        if (fstat(fd, &st) != 0) return nullptr;
        const ino_t ino = st.st_ino;
        for (auto &m : g_dmaps)
            if (m.fd == fd && m.ino == ino && m.len >= len) return m.p;
        int slot = -1;
        for (int i = 0; i < 6; i++)
            if (g_dmaps[i].fd < 0) { slot = i; break; }
        if (slot < 0) {
            // take a slot that is not the current fd, preferring the smallest mapping
            slot = 0;
            for (int i = 1; i < 6; i++)
                if (g_dmaps[i].fd != fd && g_dmaps[i].len < g_dmaps[slot].len) slot = i;
            if (g_dmaps[slot].fd == fd) slot = 0;
        }
        if (g_dmaps[slot].p) munmap(g_dmaps[slot].p, g_dmaps[slot].len);
        void *p = mmap(nullptr, len, PROT_READ, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) { g_dmaps[slot].fd = -1; g_dmaps[slot].p = nullptr; return nullptr; }
        g_dmaps[slot] = {fd, len, (uint8_t *)p, ino};
        return (uint8_t *)p;
    }

    // Pure CPU copy from wl_shm into the ANativeWindow, swapping R and B when needed.
    void blit(wl_resource *buffer) {
        const uint32_t __t0 = now_ms();
        if (!buffer || !wl_resource_get_user_data(buffer)) {
            if (buffer) wl_buffer_send_release(buffer);
            return;
        }
        // Not in the active workspace: do not draw and do not show. This has to be blocked
        // here, because place() is reached from the client's redraw path and re-shows the layer.
        if (!ws_visible(this)) { wl_buffer_send_release(buffer); return; }
        if (is_cursor) g_have_client_cursor = true;
        // Clamp into the output only on the first map. Doing it on every layer rebuild
        // yields x = 0 as soon as the buffer reaches the output width, which makes the
        // window jump to the top left corner when a zoom ends.
        const bool first_map = !mapped;
        // Xwayland root window: headless. The buffer still has to be released or Xwayland stalls.
        if (!is_x_root && strstr(title, "Xwayland on ") != nullptr) {
            is_x_root = true;
            printf("[wm] '%s' is an X11 root window -> headless, no layer created\n", title);
        }
        /* is_x_root: blit normally, otherwise the root window stays blank */
        uint32_t magic = *(uint32_t *)wl_resource_get_user_data(buffer);
        if (magic == DMABUF_MAGIC) {
            DmabufBuf *db = (DmabufBuf *)wl_resource_get_user_data(buffer);
            static int dmabuf_logs = 0;
            if (dmabuf_logs++ < 8)
                printf("[dmabuf] %s %dx%d fmt=0x%08x mod=0x%llx planes=%d "
                       "fd0=%d stride0=%u off0=%u\n",
                       is_subsurface ? "sub" : "main", db->w, db->h, db->drm_format,
                       (unsigned long long)db->modifier, db->nplanes,
                       db->fds[0], db->stride[0], db->offset[0]);

            // Linear dmabuf: mmap and use the same CPU copy path as wl_shm
            // (gbm and llvmpipe fall back to LINEAR allocations, which are CPU readable).
            if (db->nplanes == 1 && db->fds[0] >= 0 && db->w > 0 && db->h > 0 && db->stride[0] > 0) {
                sync_wait_acquire(this);   // explicit sync: wait for the client acquire point
                // Wait for the GPU to finish. A timeout is not accepted here: using an
                // unfinished buffer shows the parts that are already written on top of the
                // previous frame, which reads as a darkened or torn region.
                const bool fence_ok = dbuf_wait_fence(db->fds[0], 200);
                if (!fence_ok) {
                    static int nf = 0;
                    if (nf++ < 20)
                        printf("[sync] fence wait failed (fd=%d), frame may be incomplete\n", db->fds[0]);
                }
                // Prefer the GPU whenever it is available: the driver waits on the dma_resv
                // fence itself and the stride and alpha handling stays on the GPU.
                const bool use_gl = glc::ready();
                const size_t len = (size_t)db->stride[0] * (size_t)db->h;
                uint8_t *map = use_gl ? nullptr : dmap_get(db->fds[0], len);   // cached, no per-frame mmap
                if (!use_gl && !map) {
                    static int mfail = 0;
                    if (mfail++ < 4) printf("[dmabuf] mmap failed: %s\n", strerror(errno));
                } else {
                    struct dma_buf_sync ds;
                    memset(&ds, 0, sizeof(ds));
                    ds.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
                    ioctl(db->fds[0], DMA_BUF_IOCTL_SYNC, &ds);

                    if (!layer.win || layer.cap_w < g_out_w || layer.cap_h < g_out_h) {
                        const int32_t cw = g_out_w, ch = g_out_h;
                        if (layer.win) layer.hide();
                        if (!layer.create(title[0] ? title : "sfwm-dmabuf", cw, ch, 5 /*BGRA_8888*/)) {
                            wl_buffer_send_release(buffer);
                            return;
                        }
                        layer.cap_w = cw; layer.cap_h = ch;
                        bw = db->w; bh = db->h;
                        if (first_map) fit();
                        // The GPU path never calls setBuffersGeometry: it rebuilds the
                        // BufferQueue, and SurfaceFlinger can go one frame without a buffer.
                        // Instead the SF buffer stays at output size, the content is written
                        // at the top left and setGeometry crops it.
                        if (!use_gl)
                            ANativeWindow_setBuffersGeometry(layer.win, db->w, db->h, 5 /*BGRA_8888*/);
                        const int32_t gw = db->w < cw ? db->w : cw, gh = db->h < ch ? db->h : ch;
                        apply_geometry_ex(db->w, db->h);
                        place();
                        mapped = true;
                        notify_mapped();
                        printf("[sfwm] map(dmabuf) '%s' %dx%d (cap %dx%d) @ (%d,%d) z=%d\n",
                               title[0] ? title : (app_id[0] ? app_id : "?"), db->w, db->h, cw, ch, x, y, g_base_z + z);
                        if (!g_srv.kb_focus) kb_set_focus(this);
                    } else if (db->w != bw || db->h != bh) {
                        if (zoom != 1.f)
                            printf("[wm] dmabuf size changed to %dx%d, zoom=%.2f reset\n",
                                   db->w, db->h, zoom);
                        bw = db->w; bh = db->h;
                        // Reset the zoom: leaving it set after a resize desynchronises
                        // ew()/eh() from the real geometry and keeps the window at the
                        // negative coordinates computed while it was enlarged.
                        zoom = 1.f;
                        for (View *c : subs) if (c->mapped) { c->zoom = 1.f; c->apply_zoom(); }
                        // Update the geometry as soon as the size changes. Throttling this
                        // to 200 ms cropped the source rectangle with the previous size,
                        // which pulled in unwritten (alpha 0) pixels.
                        apply_geometry_ex(db->w, db->h);
                        // Keep the buffer size in step. The GPU path no longer touches the
                        // BufferQueue, so only the CPU path needs the 8 px debounce.
                        if (!use_gl &&
                            (abs(db->w - geom_buf_w) >= 8 || abs(db->h - geom_buf_h) >= 8)) {
                            geom_buf_w = db->w;
                            geom_buf_h = db->h;
                        // setBuffersGeometry rebuilds the BufferQueue, during which
                        // SurfaceFlinger may miss a frame, so it is logged when requested.
                            if (getenv("SFWM_GLDBG"))
                                printf("[geombuf] '%s' %dx%d -> %dx%d (gl=%d)\n",
                                       title[0] ? title : "view",
                                       layer.cap_w, layer.cap_h, db->w, db->h, (int)use_gl);
                            ANativeWindow_setBuffersGeometry(layer.win, db->w, db->h, 5 /*BGRA_8888*/);
                        }
                        // Subsurface offsets were computed with the old zoom: reposition.
                        place();
                        // Clamp again, otherwise small windows stay at the zoomed-in position.
                        if (x < 0 || y < 0 || x + ew() > g_out_w || y + eh() > g_out_h)
                            clamp_on_screen();
                    }

                    ANativeWindow_Buffer ab;
                    const uint32_t __tlock = now_ms();
                    if (ANativeWindow_lock(layer.win, &ab, nullptr) == 0) {
                        // ---- GPU composition path (default)
                        // The client dmabuf never reaches the CPU: EGLImage to GL texture,
                        // drawn over the whole SF buffer, then read back into ab.bits. Every
                        // pixel of the destination is written, so alpha stays opaque.
                        if (use_gl) {
                            static int gframes = 0;
                            const uint32_t __tg = now_ms();
                            const bool okg = glc::draw(db->fds[0], db->w, db->h,
                                                       db->drm_format, db->stride[0],
                                                       db->offset[0], db->modifier,
                                                       ab.bits,
                                                       // Write only the top left db->w x db->h, with
                                                       // the full-screen stride; setGeometry crops the rest.
                                                       (int)db->w, (int)db->h, ab.stride,
                                                       !is_cursor);
                            if (!okg) {
                                // Fallback: fill with opaque black (0xFF000000). Leaving alpha
                                // at 0 lets SurfaceFlinger blend the desktop through.
                                for (int yy = 0; yy < ab.height; yy++) {
                                    uint32_t *d = (uint32_t *)((uint8_t *)ab.bits +
                                                   (size_t)yy * (size_t)ab.stride * 4);
                                    for (int xx = 0; xx < ab.stride; xx++) d[xx] = 0xFF000000u;
                                }
                            }
                            ANativeWindow_unlockAndPost(layer.win);
                            if (getenv("SFWM_FRAMETIME") && (++gframes % 30) == 0)
                                printf("[perf] gl %dx%d -> ab=%dx%d/%d composite and submit %u ms\n",
                                       db->w, db->h, ab.width, ab.height, ab.stride,
                                       now_ms() - __tg);
                            if ((++frames % 120) == 1)
                                printf("[sfwm] '%s' gl frame %d %dx%d\n",
                                       title[0] ? title : "view", frames, db->w, db->h);
                            sync_signal_release(this);
                            wl_buffer_send_release(buffer);
                            return;
                        }
                        // ---- CPU fallback path (SFWM_GL=0)
                        { static uint32_t last = 0; static int n = 0;
                          const uint32_t dt = now_ms() - __tlock;
                          if (getenv("SFWM_FRAMETIME") && dt >= 8 && n++ < 12)
                              printf("[perf] lock blocked %u ms (%dx%d)\n", dt, db->w, db->h);
                          last = dt; (void)last; }
                        const int win_w = ANativeWindow_getWidth(layer.win);
                        const int win_h = ANativeWindow_getHeight(layer.win);
                        int rows = ab.height < db->h ? ab.height : db->h;
                        if (win_h > 0 && rows > win_h) rows = win_h;
                        int cols = ab.width < db->w ? ab.width : db->w;
                        if (win_w > 0 && cols > win_w) cols = win_w;
                        if (ab.stride > 0 && cols > ab.stride) cols = ab.stride;
                        const size_t dst_row = (size_t)ab.stride * 4;
                        { static int dg = 0;
                          if (getenv("SFWM_DMADBG") && dg < 600 && ((dg < 40) || (dg % 20) == 0))
                              printf("[dmadbg] db=%dx%d s0=%u off=%u | ab=%dx%d stride=%d "
                                     "| win=%dx%d | rows=%d cols=%d dst_row=%zu fmt=0x%08x\n",
                                     db->w, db->h, db->stride[0], db->offset[0],
                                     ab.width, ab.height, ab.stride, win_w, win_h,
                                     rows, cols, dst_row, db->drm_format);
                          dg++; }
                        // The layer is BGRA_8888, so AR24/XR24 in memory is already B,G,R,A
                        // and needs no swap; only AB24/XB24 (R,G,B,A in memory) does.
                        const bool swap = (db->drm_format == 0x34324241u /*AB24*/ ||
                                           db->drm_format == 0x34324258u /*XB24*/);
                        if (!swap && cols == ab.width && db->stride[0] == (uint32_t)(ab.stride * 4)) {
                            // Fast path: copy whole 32-bit words and force alpha to 0xFF.
                            // SurfaceFlinger selects blending from the buffer format alone and
                            // BGRA_8888 carries alpha, which GL leaves at arbitrary values, so
                            // every frame blended differently and the region flickered.
                            const uint32_t *s32 = (const uint32_t *)(map + db->offset[0]);
                            uint32_t *d32 = (uint32_t *)ab.bits;
                            const size_t words = (size_t)rows * (size_t)ab.stride;
                            for (size_t i = 0; i < words; i++) d32[i] = s32[i] | 0xFF000000u;
                        } else
                        for (int yy = 0; yy < rows; yy++) {
                            const uint8_t *sp = map + (size_t)db->offset[0] + (size_t)yy * (size_t)db->stride[0];
                            uint8_t       *dp = (uint8_t *)ab.bits + (size_t)yy * dst_row;
                            if (swap) {
                                // B,G,R,A to R,G,B,A in one 32-bit swap per pixel; the bytewise
                                // version costs 3.3M iterations per frame at 1212x2740.
                                uint32_t *d32 = (uint32_t *)dp;
                                const uint32_t *s32 = (const uint32_t *)sp;
                                for (int xx = 0; xx < cols; xx++) {
                                    const uint32_t v = s32[xx];
                                    d32[xx] = (v & 0xFF00FF00u) |
                                              ((v & 0x00FF0000u) >> 16) |
                                              ((v & 0x000000FFu) << 16);
                                }
                            } else {
                                memcpy(dp, sp, (size_t)cols * 4);
                                uint32_t *o = (uint32_t *)dp;
                                for (int xx = 0; xx < cols; xx++) o[xx] |= 0xFF000000u;
                            }
                        }
                        if (!is_cursor && testpat_mode())
                draw_testpat(ab, g_tp_tick++);
            // Pixels that were not written keep alpha 0 from SurfaceFlinger's freshly
            // allocated buffer, which blends as "show the desktop" and reads as a white
            // flash. Fill them with opaque black.
            if (rows < ab.height || cols < ab.stride) {
                for (int yy = 0; yy < rows; yy++) {
                    uint32_t *d = (uint32_t *)((uint8_t *)ab.bits + (size_t)yy * dst_row);
                    for (int xx = cols; xx < ab.stride; xx++) d[xx] = 0xFF000000u;
                }
                for (int yy = rows; yy < ab.height; yy++) {
                    uint32_t *d = (uint32_t *)((uint8_t *)ab.bits + (size_t)yy * dst_row);
                    for (int xx = 0; xx < ab.stride; xx++) d[xx] = 0xFF000000u;
                }
            }
            ANativeWindow_unlockAndPost(layer.win);
            if (getenv("SFWM_FRAMETIME") && (frames % 30) == 0)
                printf("[perf] %s %dx%d copy and submit %u ms\n",
                       is_subsurface ? "sub" : "main", db->w, db->h, now_ms() - __t0);
                        if ((++frames % 30) == 1)
                            printf("[sfwm] '%s' dmabuf frame %d %dx%d\n",
                                   title[0] ? title : "view", frames, db->w, db->h);
                    } else
                        printf("[dmabuf] lock failed\n");

                    ds.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
                    ioctl(db->fds[0], DMA_BUF_IOCTL_SYNC, &ds);
                    // No munmap here: dmap_get() keeps the mapping for reuse.
                }
                sync_signal_release(this); // explicit sync: hand the buffer back to the client
            }
            wl_buffer_send_release(buffer);
            return;
        }
        ShmBuf *sb = (ShmBuf *)wl_resource_get_user_data(buffer);
        if (!sb || sb->magic != SHMBUF_MAGIC || !sb->pool || !sb->pool->base) {
            // dmabuf, unknown format or a released pool: unsupported, release the buffer
            wl_buffer_send_release(buffer);
            return;
        }
        const int32_t sw = sb->w, sh = sb->h, stride = sb->stride;
        const uint32_t fmt = sb->format;

        // Calibrate the real render scale once. Clients using fractional scale often do
        // not follow set_buffer_scale, and every logical/physical conversion then drifts.
        // The calibration is deliberately conservative: one shot only, integer factors only,
        // and capped at 4x, because a wrong factor also corrupts pointer coordinates.
        if (!is_subsurface && !is_cursor && !scale_calibrated && req_w > 0 && sw >= req_w) {
            scale_calibrated = true;
            if (sw % req_w == 0) {
                const int32_t r = sw / req_w;
                if (r >= 2 && r <= 4) {
                    printf("[wm] '%s' scale calibrated %dx (buffer %d / requested %d)\n",
                           title[0] ? title : "view", r, sw, req_w);
                    buf_scale = r;
                }
            }
        }

        // Runaway protection: a client sending a buffer far larger than the output,
        // usually because it kept stale geometry, gets one request for a fitting size.
        if (!is_subsurface && !is_cursor && !fit_requested &&
            (sw > g_out_w * 5 / 4 || sh > g_out_h * 5 / 4)) {
            static uint32_t last_fit = 0;
            const uint32_t nowm = now_ms();
            if (nowm - last_fit > 500) {
                last_fit = nowm;
                const int32_t bsc = buf_scale > 0 ? buf_scale : 1;
                int32_t lw = (g_out_w - 60) / bsc, lh = (g_out_h - 60) / bsc;
                if (lw < 200) lw = 200;
                if (lh < 200) lh = 200;
                if (lw != req_w || lh != req_h) {
                    req_w = lw; req_h = lh;
                    toplevel_send_configure(this);
                    printf("[wm] '%s' %dx%d far larger than the output -> requesting %dx%d logical\n",
                           title[0] ? title : "view", sw, sh, lw, lh);
                }
            }
        }

        // Layer capacity and visible geometry are separate: hiding and recreating the
        // layer on every size change rebuilt it per frame during a gesture and flickered.
        if (!layer.win || layer.cap_w < g_out_w || layer.cap_h < g_out_h) {
            // Reserve the output size once. Anything larger than the output is invisible
            // anyway, so the layer never has to be rebuilt, not even while zooming.
            const int32_t cw = g_out_w, ch = g_out_h;
            if (layer.win) layer.hide();
            if (!layer.create(title[0] ? title : "sfwm-view", cw, ch, 5 /*BGRA_8888*/)) return;
            layer.cap_w = cw; layer.cap_h = ch;
            bw = sw; bh = sh;
            zoom = 1.f;
            if (first_map) fit();
            ANativeWindow_setBuffersGeometry(layer.win, cw, ch, 5 /*BGRA_8888*/);
            const int32_t gw = sw < cw ? sw : cw, gh = sh < ch ? sh : ch;
            apply_geometry_ex(sw, sh);
            // The layer has to be shown and applied before ANativeWindow_lock can succeed.
            // Doing this in the wrong order fails inside lock.
            place();
            mapped = true;
            notify_mapped();
            printf("[sfwm] map '%s' %dx%d (cap %dx%d) @ (%d,%d) z=%d\n",
                   title[0] ? title : (app_id[0] ? app_id : "?"), sw, sh, cw, ch, x, y, g_base_z + z);
            if (!g_srv.kb_focus) kb_set_focus(this);   // keyboard focus on first map
        } else if (sw != bw || sh != bh) {
            // Size changed: adjust the crop only, do not rebuild the layer.
            if (zoom != 1.f)
                printf("[wm] buffer size changed to %dx%d during a gesture, zoom=%.2f reset\n",
                       sw, sh, zoom);
            bw = sw; bh = sh;
            // The client redrew at the configured size, so the composer-side zoom is done.
            zoom = 1.f;
            for (View *c : subs) if (c->mapped) { c->zoom = 1.f; c->apply_zoom(); }
            const int32_t gw = sw < layer.cap_w ? sw : layer.cap_w;
            const int32_t gh = sh < layer.cap_h ? sh : layer.cap_h;
            apply_geometry_ex(sw, sh);
            // Subsurface offsets were computed with the old zoom: reposition and clamp,
            // otherwise small windows stay at the position they had while enlarged.
            place();
            if (x < 0 || y < 0 || x + ew() > g_out_w || y + eh() > g_out_h)
                clamp_on_screen();
            printf("[sfwm] resize '%s' %dx%d @ (%d,%d)\n",
                   title[0] ? title : "view", sw, sh, x, y);
        }

        const uint8_t *src = sb->pool->base + sb->offset;
        if (frames < 3)
            printf("[buf] %s%s src=%p %dx%d stride=%d fmt=%d\n",
                   is_subsurface ? "sub:" : "main:",
                   title[0] ? title : "?", (const void *)src, sw, sh, stride, (int)fmt);



        ANativeWindow_Buffer ab;
        if (ANativeWindow_lock(layer.win, &ab, nullptr) == 0) {
            const int win_w = ANativeWindow_getWidth(layer.win);
            const int win_h = ANativeWindow_getHeight(layer.win);
            static int lock_dbg = 0;
            if (lock_dbg++ < 6)
                printf("[lock] '%s' buf=%dx%d stride=%d fmt=%d bits=%p | win=%dx%d src=%dx%d\n",
                       title[0] ? title : "view", ab.width, ab.height, ab.stride,
                       ab.format, (void *)ab.bits, win_w, win_h, sw, sh);

            // Clamp against the client buffer, the window size and the stride.
            int rows = ab.height < sh ? ab.height : sh;
            if (win_h > 0 && rows > win_h) rows = win_h;
            int cols = ab.width < sw ? ab.width : sw;
            if (win_w > 0 && cols > win_w) cols = win_w;
            if (ab.stride > 0 && cols > ab.stride) cols = ab.stride;
            const size_t dst_row = (size_t)ab.stride * 4;   // ANativeWindow_Buffer.stride is in pixels
            for (int yy = 0; yy < rows; yy++) {
                const uint8_t *s = src + (size_t)yy * stride;
                uint8_t *d = (uint8_t *)ab.bits + (size_t)yy * dst_row;
                if (fmt == WL_SHM_FORMAT_ARGB8888) {
                    // Layer is BGRA_8888 and ARGB8888 in memory is B,G,R,A: copy and force alpha
                    const uint32_t *s32 = (const uint32_t *)s;
                    uint32_t *d32 = (uint32_t *)d;
                    for (int xx = 0; xx < cols; xx++) d32[xx] = s32[xx] | 0xFF000000u;
                } else if (fmt == WL_SHM_FORMAT_XRGB8888) {
                    // The X byte is not an alpha value, so force opacity.
                    for (int xx = 0; xx < cols; xx++) {
                        d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 0xFF;
                        s += 4; d += 4;
                    }
                } else {
                    memcpy(d, s, (size_t)cols * 4);
                }
            }
            ANativeWindow_unlockAndPost(layer.win);
            if (getenv("SFWM_FRAMETIME") && (frames % 30) == 0)
                printf("[perf] %s %dx%d copy and submit %u ms\n",
                       is_subsurface ? "sub" : "main", sw, sh, now_ms() - __t0);
            if (g_in_drag && (frames % 5) == 1)
                printf("[move] client still redrawing during a drag, frame=%d\n", frames);
            if ((++frames % 30) == 1)
                printf("[sfwm] '%s' frame %d  %dx%d stride=%d fmt=%d\n",
                       title[0] ? title : "view", frames, ab.width, ab.height, ab.stride, ab.format);
        } else {
            printf("[sfwm] lock failed\n");
        }

        wl_buffer_send_release(buffer);
    }

    bool commit() {
        // A dmabuf is never read one frame late. The swapchain holds two or three buffers,
        // so the client starts drawing frame N+2 into the buffer we are still holding, and
        // the copy we read is an interleaved mix of both frames.
        bool pend_is_dmabuf = false;
        if (pending_buffer) {
            void *ud = wl_resource_get_user_data(pending_buffer);
            if (ud && *(uint32_t *)ud == DMABUF_MAGIC) pend_is_dmabuf = true;
        }
        // Escape hatch: an environment variable restores the old behaviour.
        if (pending_buffer && (pend_is_dmabuf || getenv("SFWM_NO_DEFER"))) {
            blit(pending_buffer);
            pending_buffer = nullptr;
            pres_send_presented(this);
        }
        if (pending_buffer) {
        // The buffer read here arrived one commit ago, so the client GPU work for it is
        // certainly complete.
            if (read_next) {
                if (hold) { wl_list_remove(&hold->lis.link); delete hold; hold = nullptr; }
                blit(read_next);
                read_next = nullptr;
            }
            read_next = pending_buffer;
            read_next_ms = now_ms();
            if (read_next) {
                hold = new HoldListener{};
                hold->view = this;
                hold->lis.notify = [](wl_listener *l, void *) {
                    HoldListener *h = wl_container_of(l, h, lis);
                    if (h->view) h->view->read_next = nullptr;
                    wl_list_remove(&l->link);
                    delete h;
                };
                wl_resource_add_destroy_listener(read_next, &hold->lis);
            }
            pending_buffer = nullptr;
            pres_send_presented(this);
        }
        // Do not send wl_callback.done from here: it grants an unlimited frame rate.
        // The 16 ms timer releases the callbacks; see the note at the top of the file.
        frame_defer(this);
        return true;
    }
};

// Queue frame callbacks into 16 ms batches (defined here because it needs the View type)
static void frame_defer(View *v) {
    if (!v || !v->frame_cb) return;
    FrameSlot *s = new FrameSlot{};
    s->cb = v->frame_cb;
    s->lis.notify = frame_cb_gone;
    wl_resource_add_destroy_listener(v->frame_cb, &s->lis);
    g_frame_pending.push_back(v->frame_cb);
    v->frame_cb = nullptr;
    if (!g_frame_armed) {                    // arm on demand
        g_frame_armed = true;
        if (g_frame_timer) wl_event_source_timer_update(g_frame_timer, 16);
    }
}
