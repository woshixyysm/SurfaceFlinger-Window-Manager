// view.hpp —— 一个 wl_surface 的全部状态 + 几何/堆叠助手
//
// 从 sfwm.cpp 原样搬出（只搬不改）。它依赖包含顺序：
//   <sf_layer.hpp> / <wayland-server> / <vector> / Server 的前向声明 /
//   g_out_w..g_ime_on..g_reason 这些全局，必须在 include 本文件**之前**出现。
// 所以 sfwm.cpp 里保留它原来的位置：#include "view.hpp" 插在原来 struct 的位置。
#pragma once

struct View {
    Server *srv = nullptr;
    wl_resource *surface = nullptr;
    wl_resource *xdg_surface = nullptr;
    wl_resource *toplevel = nullptr;
    wl_resource *pending_buffer = nullptr;
    // ★ 延后一帧读取（GPU 写竞争）：客户端提交的下一帧到达时，才去读上一帧。
    //   原因：客户端（OBS 预览 = 子表面，60fps）用 GPU 渲染，我们一收到 commit
    //   就立刻 mmap 读，读到的是“画一半”的缓冲区 → 预览区和红框周期性闪。
    //   我们自己的合成图案不闪，正是因为它是 CPU 写完的、不参与这个竞争。
    //   拖一帧（约 16ms 延迟）后，那一帧的 GPU 工作肯定已完成。
    wl_resource *read_next = nullptr;
    uint32_t read_next_ms = 0;   // 攥住这一帧的时刻（看门狗用）
    // read_next 是“跨 commit 攥着”的资源指针，客户端可能在 resize/swapchain 重建时
    // 直接销毁这个 wl_buffer → 指针悬空 → blit 里 user_data 为 NULL 直接段错误。
    // 所以挂一个销毁监听，被销毁时立刻清空。
    struct HoldListener { wl_listener lis; View *view; };
    HoldListener *hold = nullptr;
    wl_resource *frame_cb = nullptr;
    sf::Layer layer;
    int32_t bw = 0, bh = 0;            // 当前 buffer 尺寸
    int32_t req_w = 0, req_h = 0;      // 拖边角时要求的逻辑尺寸（0 = 不限制）
    int32_t x = 0, y = 0;
    int32_t z = 0;
    // 仅手势期间 ≠ 1 的「合成器侧缩放」：只改图层 dst 几何，不动客户端 buffer、
    // 不发 configure。这就是 Hyprland 那套做法 —— 客户端毫无感知，所以不可能闪。
    float zoom = 1.f;
    bool mapped = false;
    bool is_cursor = false;
    bool is_popup = false;
    bool is_ime = false;        // 输入法（org.sfwm.ime）：永远置顶，且按状态真隐藏

    // ---- zwlr_layer_shell_v1（面板/托盘/启动器）
    bool is_layer = false;
    wl_resource *layer_surface = nullptr;
    uint32_t layer_anchor = 0;
    uint32_t layer_kb = 0;          // keyboard_interactivity
    uint32_t layer_num = 3;         // 1=background 2=bottom 3=top 4=overlay
    int32_t layer_w = 0, layer_h = 0;      // 客户端请求的逻辑尺寸（0 = 自己定）
    int32_t layer_excl = 0;
    int32_t layer_mt = 0, layer_mr = 0, layer_mb = 0, layer_ml = 0;
    char layer_ns[64] = {0};

    // ---- wp_presentation_feedback 待回执
    std::vector<wl_resource *> feedbacks;

    // ---- 杂项扩展
    uint32_t content_type = 0;      // wp_content_type_v1
    int32_t  alpha_mult   = 256;    // wp_alpha_modifier_v1，fixed 1.0

    // ---- 跟踪用：上次上报的几何
    int32_t last_x = -0x7fffffff, last_y = -0x7fffffff, last_z = -0x7fffffff;

    // ---- wp_linux_drm_syncobj_v1（显式同步）
    wl_resource *syncobj_surface = nullptr;
    uint32_t sync_acq_handle = 0, sync_rel_handle = 0;
    uint64_t sync_acq_point  = 0, sync_rel_point  = 0;
    bool     sync_has_acq = false, sync_has_rel = false;
    // Xwayland 的根窗口（"Xwayland on :0"）= X11 的屏幕背景，没任何内容可画。
    // 让它无头：不建图层、不参与堆叠、不参与 IME 让位计算（它在 y=0 会把 let位 钳成 0）。
    bool is_x_root = false;
    bool want_maximized = false;
    bool want_fullscreen = false;
    View *parent = nullptr;
    int32_t buf_scale = 1;      // wl_surface.set_buffer_scale；surface 局部坐标 = 物理 / buf_scale
    bool    scale_calibrated = false;   // 自校准只做一次，避免把中间态当成倍率
    bool    fit_requested    = false;   // “请缩小”只发一次，避免和客户端互相拉扯
    int frames = 0;
    char title[128] = {0};
    char app_id[128] = {0};

    // ---- wl_subcompositor
    bool is_subsurface = false;
    View *sub_parent = nullptr;
    int32_t sub_x = 0, sub_y = 0;      // 相对父 surface 的偏移（surface 局部坐标）
    std::vector<View *> subs;          // 子表面

    // 堆叠键：父 z*8，子表面占父之上的一个槽位
    int32_t stack_key() const {
        if (is_subsurface && sub_parent) return sub_parent->z * 8 + 4;
        return z * 8;
    }

    // 键盘弹出时，窗口可见高度只能到「键盘上沿」为止。
    // 比屏幕还高的窗口靠位移救不了，必须裁剪 —— 否则底部永远盖着键盘。
    int32_t vis_h_for(int32_t sh) const {
        // ★ 只有键盘**真的显示中**才让位。
        //   以前只看 g_ime_h > 0 —— 我们自己的 IME 客户端上报过一次高度后，
        //   即使键盘已经收起，g_ime_h 还是个非 0 的陈旧值，
        //   于是所有窗口底部被永久硬裁掉一截（而且看起来像“变扁”）。
        // ★ 默认**不给键盘让位**。
        //   我们没法可靠判断键盘到底显示没有：IME 客户端上报过一次高度之后，
        //   即使键盘已经收起，g_ime_on / g_ime_h 也可能留着旧值 ——
        //   一旦判断错，窗口底部就被永久裁掉一截（正是用户看到的“没出键盘也在裁”）。
        //   宁可让键盘盖住窗口底部。需要旧行为时设 SFWM_IME_RESERVE=1。
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
            // ★ 父表面被手势放大时，子表面的偏移也必须一起放大。
            //   Qt/OBS 把真正的内容画在子表面里（父表面基本是空的），
            //   只缩父层的结果就是“父层放大了、内容没动” → 内容跑到左上角。
            const float pz = sub_parent->zoom > 0.f ? sub_parent->zoom : 1.f;
            x = sub_parent->x + (int32_t)((float)sub_x * sub_parent->buf_scale * pz);
            y = sub_parent->y + (int32_t)((float)sub_y * sub_parent->buf_scale * pz);
            z = sub_parent->z;
        }
        // 键盘占了底部 g_ime_h：不做位移，而是把图层的可见高度裁到「键盘上沿以上」。
        // 位移在有窗口比屏幕高的时候根本救不了 —— 窗口一移上去，底部照样盖着键盘。
        const int32_t shift = 0;
        // 输入法永远压在一切之上（和 xdg_popup / 全屏底图规则无关）
        const int32_t zz = g_base_z + (is_ime ? 5000 : stack_key());
        layer.place(x, y - shift, zz);
        // 普通窗口一律不透明：省掉 alpha 混合（也修掉客户端 alpha 不稳定造成的闪烁）
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
        g_reason = "?";   // 不粘滞：只标记产生这次放置的那一步
        for (View *c : subs) if (c->mapped) c->place();
    }

    // 把手势缩放落到图层几何上（纯合成器侧，SurfaceFlinger 用 GPU 采样缩放）。
    //   src = 原始 buffer 整块，dst = buffer 尺寸 × zoom
    //   客户端完全不知道，因此不会重建 swapchain，也就不会闪。
    // 把「手势缩放 + 键盘裁剪」一次性落到图层几何上。
    //   ⚠️ 键盘裁剪必须**同时**裁 src 和 dst：只把 dst 高度改小 = 把整幅画面压扁
    //      （SurfaceFlinger 的 toDisplayTransform 会变成 y=0.56 这种垂直缩放）。
    void apply_geometry_ex(int32_t src_w, int32_t src_h) {
        if (!layer.win || src_w <= 0 || src_h <= 0) return;
        const float z = (zoom > 0.f ? zoom : 1.f);
        int32_t dw = (int32_t)(src_w * z + 0.5f);
        int32_t dh = (int32_t)(src_h * z + 0.5f);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        int32_t vh = vis_h_for(dh);          // 键盘上沿以上还剩多少像素
        if (vh < 1) vh = 1;
        int32_t src_hh = src_h;
        if (vh < dh) {
            src_hh = (int32_t)((int64_t)src_h * vh / dh);
            if (src_hh < 1) src_hh = 1;
        }
        layer.setGeometry(sf::Rect{0, 0, src_w, src_hh},
                          sf::Rect{0, 0, dw, vh});
    }

    void apply_zoom() { apply_geometry_ex(bw, bh); }

    // 递归设置缩放：父表面缩放时，子表面（Qt 的内容面）必须同步缩放，
    //   否则父层放大了、内容层没动，视觉上就是“内容跑到左上角”。
    void set_zoom(float z) {
        // ★ 全程攒在一个 SurfaceFlinger 事务里，最后一次性 apply。
        //   以前 place()/setGeometry() 各自 apply → 一帧内父层、子层、位置、几何
        //   分好几次提交，SF 会在中间状态合成 → 缩放时逐帧闪。
        sf::begin_batch();
        zoom = z;
        apply_zoom();
        for (View *c : subs) {
            if (!c->mapped) continue;
            c->zoom = z;
            c->apply_zoom();
        }
        place();          // place() 内部会递归给子表面定位（同样攒在事务里）
        sf::end_batch();
    }

    // 手势缩放后的“有效尺寸”（= 图层 dst 尺寸，也是命中测试要用的尺寸）
    int32_t ew() const { return (int32_t)(bw * (zoom > 0.f ? zoom : 1.f) + 0.5f); }
    int32_t eh() const { return (int32_t)(bh * (zoom > 0.f ? zoom : 1.f) + 0.5f); }
    // 屏幕坐标 -> surface 局部坐标。缩放期间分母必须乘上 zoom，
    // 否则“放大后点得到的地方”和“视觉位置”会错位（老 bug 的根源之一）。
    int32_t to_local_x(int32_t sx) const {
        const float s = (float)(buf_scale > 0 ? buf_scale : 1) * (zoom > 0.f ? zoom : 1.f);
        return (int32_t)((sx - x) / s);
    }
    int32_t to_local_y(int32_t sy) const {
        const float s = (float)(buf_scale > 0 ? buf_scale : 1) * (zoom > 0.f ? zoom : 1.f);
        return (int32_t)((sy - y) / s);
    }

    void map() {
        if (mapped) return;
        g_reason = "map";
        mapped = true;
        if (!is_x_root && strstr(title, "Xwayland on ") != nullptr) is_x_root = true;
        // 输入法：app_id/title 里带 sfwm.ime 的就是我们自己那个键盘
        if (!is_ime && (strstr(app_id, "sfwm.ime") || strstr(title, "sfwm-ime") ||
                        strstr(app_id, "org.sfwm.ime"))) {
            is_ime = true;
            printf("[wm] '%s' 识别为输入法 -> 永久置顶\n", title[0] ? title : app_id);
        }
        if (is_x_root)
            printf("[wm] X11 root window -> background layer\n");
        if (is_layer) layer_apply_anchor(this);
        place();
        if (!is_subsurface && !is_cursor) clamp_on_screen();
        // 默认层叠规则（不再按名字特判）：
        //   整屏窗口 = 「底图」（Xwayland 根窗口、login 这类很可能就是一块整屏黑），
        //     放进 z 很低的专用带、按映射先后排序，永远压在内容窗口之下；
        //   其余窗口 = 映射即置顶。
        // 不这么做的话，「后映射在上」会让一块整屏黑把 kitty 连同状态栏一起盖死。
        if (!is_subsurface && !is_cursor && !is_layer) {
            if (bw * 100 >= g_out_w * 95 && bh * 100 >= g_out_h * 95) {
                z = -1000 + (g_bg_seq++);
                place();
                printf("[wm] '%s' 整屏 %dx%d -> 记为底图 z=%d\n",
                       title[0] ? title : (app_id[0] ? app_id : "view"), bw, bh, z);
            } else {
                int32_t top = -1;
                for (View *o : g_srv.views)
                    if (o->mapped && !o->is_cursor && !o->parent && !o->is_layer && o != this && o->z > top) top = o->z;
                if (z <= top) {
                    z = top + 1;
                    place();
                    printf("[wm] '%s' 映射即置顶 z=%d\n",
                           title[0] ? title : (app_id[0] ? app_id : "view"), z);
                }
            }
        }
        printf("[sfwm] map '%s' %dx%d @ (%d,%d) z=%d\n",
               title[0] ? title : (app_id[0] ? app_id : "?"), bw, bh, x, y, g_base_z + z);
        if (!is_subsurface && !is_cursor && !is_x_root && !is_layer) kb_set_focus(this);   // 新窗口上屏就抢焦点（正常 WM 行为）
        // 层表面只有显式声明要键盘（exclusive / on_demand）才拿焦点
        if (is_layer && layer_kb != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) kb_set_focus(this);
    }

    // 新窗口上屏时塞进屏幕：
    //   1) 位置夹紧（否则 x+width 超过屏幕右边，整块被裁掉）
    //   2) 尺寸比屏幕还大就发一次 configure 让它缩小（客户端自己会重排）
    void fit() {
        if (is_subsurface || is_cursor || is_layer) return;
        g_reason = "fit";
        // 只“提示”一次尺寸，而且**绝不改位置**。
        //   老实现每次 map/尺寸变化都发 configure 去逼客户端缩小，但客户端有最小尺寸时
        //   永远缩不到 → 双方来回拉扯：buffer 4710↔5067、位置 -265→-304→-240 一直变，
        //   看起来就是“窗口自己飞走”。窗口比屏幕大是允许的（用户自己挪）。
        if (!fit_requested && (bw > g_out_w || bh > g_out_h)) {
            fit_requested = true;
            const int32_t sc = client_scale(this);
            int32_t lw = g_out_w / sc, lh = g_out_h / sc;
            if (lw < 320) lw = 320;
            if (lh < 240) lh = 240;
            req_w = lw;
            req_h = lh;
            toplevel_send_configure(this);
            printf("[wm] '%s' %dx%d 比屏幕大 -> 一次性请求 %dx%d 逻辑（之后不再纠缠）\n",
                   title[0] ? title : "view", bw, bh, lw, lh);
        }
    }

    // 把窗口拉回屏幕。
    //   能装下（bw<=屏幕宽）就**整体可见** —— 对话框的 OK/Cancel 都在右下角，
    //   只保“至少一小块可见”等于按钮永远点不到。
    //   装不下才退化成“保证 80dp 可见”。
    void clamp_on_screen() {
        // ★ 用「有效尺寸」（含手势缩放的 zoom）来夹，不能用未缩放的 bw/bh。
        //   用 bw 时：放大期间 bw==屏幕宽 → 条件恒成立 → x 被强行归 0，
        //   表现就是“每次捏合结束窗口跳到左上角”。
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

    // dmabuf mmap 缓存：客户端一份 buffer 会反复提交，每次 mmap+munmap 都很贵。
    // 这里按 fd 缓存映射，最多 6 个（屏幕 14MB/个，正常客户端 2~3 个 buffer）。
    struct DMap { int fd = -1; size_t len = 0; uint8_t *p = nullptr; };

    static uint8_t *dmap_get(int fd, size_t len) {
        static DMap g_dmaps[6];   // 函数内静态：避免类静态成员需要类外定义
        for (auto &m : g_dmaps)
            if (m.fd == fd && m.len >= len) return m.p;
        int slot = -1;
        for (int i = 0; i < 6; i++)
            if (g_dmaps[i].fd < 0) { slot = i; break; }
        if (slot < 0) slot = 0;                       // 满了就复用第 0 个（最坏多一次映射）
        if (g_dmaps[slot].p) munmap(g_dmaps[slot].p, g_dmaps[slot].len);
        void *p = mmap(nullptr, len, PROT_READ, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) { g_dmaps[slot].fd = -1; g_dmaps[slot].p = nullptr; return nullptr; }
        g_dmaps[slot] = {fd, len, (uint8_t *)p};
        return (uint8_t *)p;
    }

    // 纯 CPU 搬运：wl_shm → ANativeWindow，顺便 R/B 交换
    void blit(wl_resource *buffer) {
        const uint32_t __t0 = now_ms();
        if (!buffer || !wl_resource_get_user_data(buffer)) {
            if (buffer) wl_buffer_send_release(buffer);
            return;
        }
        if (is_cursor) g_have_client_cursor = true;
        // 只有“首次上屏”才把窗口夹进屏幕。
        // ⚠️ 以前每次重建图层都调 fit()，而 fit() 里有
        //      if (bw <= g_out_w && x + bw > g_out_w) x = g_out_w - bw;
        //    缩放时 bw 很快就等于 g_out_w，于是 x 被算成 0 ——
        //    表现就是“一缩放窗口就跳回左上角”。
        const bool first_map = !mapped;
        // Xwayland 根窗口：无头。buffer 必须还回去，否则 Xwayland 会卡死。
        if (!is_x_root && strstr(title, "Xwayland on ") != nullptr) {
            is_x_root = true;
            printf("[wm] '%s' 是 X11 根窗口 -> 无头，不建图层\n", title);
        }
        /* is_x_root: blit normally, otherwise the root window stays blank */
        uint32_t magic = *(uint32_t *)wl_resource_get_user_data(buffer);
        if (magic == DMABUF_MAGIC) {
            DmabufBuf *db = (DmabufBuf *)wl_resource_get_user_data(buffer);
            static int dmabuf_logs = 0;
            if (dmabuf_logs++ < 8)
                printf("[dmabuf] %s %dx%d fmt=0x%08x mod=0x%llx planes=%d "
                       "fd0=%d stride0=%u off0=%u\n",
                       is_subsurface ? "子" : "主", db->w, db->h, db->drm_format,
                       (unsigned long long)db->modifier, db->nplanes,
                       db->fds[0], db->stride[0], db->offset[0]);

            // 线性 dmabuf：mmap 之后走和 wl_shm 相同的纯 CPU 搬运
            // （gbm/llvmpipe 退化成 LINEAR 分配，CPU 可读）
            if (db->nplanes == 1 && db->fds[0] >= 0 && db->w > 0 && db->h > 0 && db->stride[0] > 0) {
                sync_wait_acquire(this);   // 显式同步：等客户端的 acquire 点
                const size_t len = (size_t)db->stride[0] * (size_t)db->h;
                uint8_t *map = dmap_get(db->fds[0], len);   // 缓存：别再每帧 mmap/munmap
                if (!map) {
                    static int mfail = 0;
                    if (mfail++ < 4) printf("[dmabuf] mmap 失败: %s\n", strerror(errno));
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
                        ANativeWindow_setBuffersGeometry(layer.win, cw, ch, 5 /*BGRA_8888*/);
                        const int32_t gw = db->w < cw ? db->w : cw, gh = db->h < ch ? db->h : ch;
                        apply_geometry_ex(db->w, db->h);
                        place();
                        mapped = true;
                        printf("[sfwm] map(dmabuf) '%s' %dx%d (cap %dx%d) @ (%d,%d) z=%d\n",
                               title[0] ? title : (app_id[0] ? app_id : "?"), db->w, db->h, cw, ch, x, y, g_base_z + z);
                        if (!g_srv.kb_focus) kb_set_focus(this);
                    } else if (db->w != bw || db->h != bh) {
                        if (zoom != 1.f)
                            printf("[wm] ⚠ dmabuf 尺寸变了 %dx%d，zoom=%.2f 被清零\n",
                                   db->w, db->h, zoom);
                        bw = db->w; bh = db->h;
                        // ★ 这一支以前**从不**清 zoom：客户端按新尺寸重绘后 zoom 还残留，
                        //   于是 ew()/eh()（命中测试用）和图层实际几何对不上，
                        //   位置也还停在放大时算出来的负坐标里
                        //   → 表现就是“缩放手势一结束，窗口跑到左上角”。
                        zoom = 1.f;
                        for (View *c : subs) if (c->mapped) c->zoom = 1.f;
                        // 手势期间客户端 swapchain 正在重建，我们每帧改可见区域
                        // = 旧帧的裁剪框 + 新帧内容，看着就是闪。限流到 200ms。
                        static uint32_t last_geo = 0;
                        const uint32_t nowg = now_ms();
                        if (!g_in_gesture || nowg - last_geo >= 200) {
                            last_geo = nowg;
                            const int32_t gw = db->w < layer.cap_w ? db->w : layer.cap_w;
                            const int32_t gh = db->h < layer.cap_h ? db->h : layer.cap_h;
                            apply_geometry_ex(db->w, db->h);
                        }
                        // 子表面偏移是按旧 zoom 算的 → 必须重新定位
                        place();
                        // 再夹一次：否则小窗口会停在放大时的负坐标处
                        if (x < 0 || y < 0 || x + ew() > g_out_w || y + eh() > g_out_h)
                            clamp_on_screen();
                    }

                    ANativeWindow_Buffer ab;
                    const uint32_t __tlock = now_ms();
                    if (ANativeWindow_lock(layer.win, &ab, nullptr) == 0) {
                        { static uint32_t last = 0; static int n = 0;
                          const uint32_t dt = now_ms() - __tlock;
                          if (getenv("SFWM_FRAMETIME") && dt >= 8 && n++ < 12)
                              printf("[perf] lock 阻塞 %u ms (%dx%d)\n", dt, db->w, db->h);
                          last = dt; (void)last; }
                        const int win_w = ANativeWindow_getWidth(layer.win);
                        const int win_h = ANativeWindow_getHeight(layer.win);
                        int rows = ab.height < db->h ? ab.height : db->h;
                        if (win_h > 0 && rows > win_h) rows = win_h;
                        int cols = ab.width < db->w ? ab.width : db->w;
                        if (win_w > 0 && cols > win_w) cols = win_w;
                        if (ab.stride > 0 && cols > ab.stride) cols = ab.stride;
                        const size_t dst_row = (size_t)ab.stride * 4;
                        // 图层已经是 BGRA_8888，AR24/XR24 在内存里就是 B,G,R,A
                        // → 完全不用交换；只有 AB24/XB24（内存 R,G,B,A）才要换。
                        const bool swap = (db->drm_format == 0x34324241u /*AB24*/ ||
                                           db->drm_format == 0x34324258u /*XB24*/);
                        if (!swap && cols == ab.width && db->stride[0] == (uint32_t)(ab.stride * 4)) {
                            // 快路径：整块按 32 位字搬，同时把 alpha 顶成 0xFF。
                            //   为什么要强制不透明：SurfaceFlinger 在 Android 10+ 已经没有
                            //   eLayerOpaque 这个标志了，它**只看 buffer 格式**。BGRA_8888 带
                            //   alpha，而客户端的 alpha 位是 GL 渲染的残留值（有时全 0），
                            //   SF 按 PREMULTIPLIED 混合 → 同一块内容每帧透明度不同 →
                            //   预览区和选框忽明忽暗（也就是我们看到的闪烁）。
                            const uint32_t *s32 = (const uint32_t *)(map + db->offset[0]);
                            uint32_t *d32 = (uint32_t *)ab.bits;
                            const size_t words = (size_t)rows * (size_t)ab.stride;
                            for (size_t i = 0; i < words; i++) d32[i] = s32[i] | 0xFF000000u;
                        } else
                        for (int yy = 0; yy < rows; yy++) {
                            const uint8_t *sp = map + (size_t)db->offset[0] + (size_t)yy * (size_t)db->stride[0];
                            uint8_t       *dp = (uint8_t *)ab.bits + (size_t)yy * dst_row;
                            if (swap) {
                                // B,G,R,A -> R,G,B,A：一次 32 位字交换，别再用 4 条字节写。
                                // (逐字节版本在 1212x2740 上每帧 330 万次迭代，直接把事件循环拖死)
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
            ANativeWindow_unlockAndPost(layer.win);
            if (getenv("SFWM_FRAMETIME") && (frames % 30) == 0)
                printf("[perf] %s %dx%d 拷贝+提交 %u ms\n",
                       is_subsurface ? "子" : "主", db->w, db->h, now_ms() - __t0);
                        if ((++frames % 30) == 1)
                            printf("[sfwm] '%s' dmabuf frame %d %dx%d\n",
                                   title[0] ? title : "view", frames, db->w, db->h);
                    } else
                        printf("[dmabuf] lock 失败\n");

                    ds.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
                    ioctl(db->fds[0], DMA_BUF_IOCTL_SYNC, &ds);
                    // 不 munmap：dmap_get() 的缓存会复用，避免每帧一次映射（很贵）
                }
                sync_signal_release(this); // 显式同步：告诉客户端这块 buffer 可以复用
            }
            wl_buffer_send_release(buffer);
            return;
        }
        ShmBuf *sb = (ShmBuf *)wl_resource_get_user_data(buffer);
        if (!sb || sb->magic != SHMBUF_MAGIC || !sb->pool || !sb->pool->base) {
            // dmabuf / 未知格式 / 池已释放，暂不支持，直接还回去
            wl_buffer_send_release(buffer);
            return;
        }
        const int32_t sw = sb->w, sh = sb->h, stride = sb->stride;
        const uint32_t fmt = sb->format;

        // 自校准真实渲染倍数。
        // Qt 用 wp_fractional_scale_v1 时经常不按 wl_surface.set_buffer_scale 来，
        // 我们以为 1 倍、它其实按 3 倍出 buffer（1272 逻辑 -> 3816 物理），
        // 于是所有“逻辑↔物理”换算都错，一拖就变成比屏幕大好几倍。
        // 用「实际 buffer ÷ 我们请求的逻辑尺寸」反推一次即可。
        // ★ 自校准必须极其保守，否则会灾难性跑飞：
        //   老代码用 (sw + req_w/2)/req_w 且只允许往上调，于是 fit() 反复缩小尺寸的
        //   中间态（buffer 4644 / 请求 663）被当成 7 倍 -> 所有指针坐标除了 7 ->
        //   点击落在错误控件上，客户端又把 press 当拖动 -> 窗口跟着指针乱飞。
        //   现在：只标定一次、必须整除、上限 4 倍。
        if (!is_subsurface && !is_cursor && !scale_calibrated && req_w > 0 && sw >= req_w) {
            scale_calibrated = true;
            if (sw % req_w == 0) {
                const int32_t r = sw / req_w;
                if (r >= 2 && r <= 4) {
                    printf("[wm] '%s' 一次标定 %dx（buffer %d / 请求 %d）\n",
                           title[0] ? title : "view", r, sw, req_w);
                    buf_scale = r;
                }
            }
        }

        // 失控保护：客户端送来的 buffer 比屏幕大太多（通常是它自己存了旧几何，
        // 或缩放时 configure 没跟上）→ 限流地重新请求一个装得下的尺寸。
        // 没有这个，窗口会越缩越大，看着就是“一下炸成比屏幕大几倍”。
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
                    printf("[wm] '%s' %dx%d 超出屏幕太多 -> 重新请求 %dx%d 逻辑\n",
                           title[0] ? title : "view", sw, sh, lw, lh);
                }
            }
        }

        // 图层的“分配尺寸”和“可见尺寸”分开：
        // 以前每次尺寸变化都 hide()+create()，缩放时每帧新建一个图层 → 屏幕狂闪。
        // 现在只在“装不下”的时候才扩容（留 25% 余量），其余情况用 setGeometry 裁。
        if (!layer.win || layer.cap_w < g_out_w || layer.cap_h < g_out_h) {
            // 容量直接按「屏幕尺寸」一次性预留。
            // 窗口比屏幕大时，超出的部分本来也看不见（被屏幕裁掉），
            // 所以图层从来不需要比屏幕大 —— 于是缩放过程中永远不会重建图层，
            // 也就不会闪。这是比“留 1.6 倍余量”干净得多的做法。
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
            // 关键：必须先把图层 show + apply 提交给 SF，之后 ANativeWindow_lock 才有效。
            // 顺序错了会在 lock 里直接崩（第一版就是这么死的）。
            place();
            mapped = true;
            printf("[sfwm] map '%s' %dx%d (cap %dx%d) @ (%d,%d) z=%d\n",
                   title[0] ? title : (app_id[0] ? app_id : "?"), sw, sh, cw, ch, x, y, g_base_z + z);
            if (!g_srv.kb_focus) kb_set_focus(this);   // 第一次上屏拿键盘焦点
        } else if (sw != bw || sh != bh) {
            // 尺寸变了：只改裁剪，不重建（不闪）
            if (zoom != 1.f)
                printf("[wm] ⚠ 手势中 buffer 尺寸变了 %dx%d，zoom=%.2f 被清零\n",
                       sw, sh, zoom);
            bw = sw; bh = sh;
            // 客户端已按 configure 的新尺寸重绘 → 合成器侧 zoom 功成身退
            zoom = 1.f;
            for (View *c : subs) if (c->mapped) c->zoom = 1.f;
            const int32_t gw = sw < layer.cap_w ? sw : layer.cap_w;
            const int32_t gh = sh < layer.cap_h ? sh : layer.cap_h;
            apply_geometry_ex(sw, sh);
            // 子表面偏移是按旧 zoom 算的 → 必须重新定位；
            // 再夹一次，否则小窗口会停在放大时的负坐标处（“跑左上角”）
            place();
            if (x < 0 || y < 0 || x + ew() > g_out_w || y + eh() > g_out_h)
                clamp_on_screen();
            printf("[sfwm] resize '%s' %dx%d @ (%d,%d)\n",
                   title[0] ? title : "view", sw, sh, x, y);
        }

        const uint8_t *src = sb->pool->base + sb->offset;
        if (frames < 3)
            printf("[buf] %s%s src=%p %dx%d stride=%d fmt=%d\n",
                   is_subsurface ? "子:" : "主:",
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

            // 三重夹紧：客户端缓冲、窗口真实尺寸、行跨度，任何一个更小就用它
            int rows = ab.height < sh ? ab.height : sh;
            if (win_h > 0 && rows > win_h) rows = win_h;
            int cols = ab.width < sw ? ab.width : sw;
            if (win_w > 0 && cols > win_w) cols = win_w;
            if (ab.stride > 0 && cols > ab.stride) cols = ab.stride;
            const size_t dst_row = (size_t)ab.stride * 4;   // ANativeWindow_Buffer.stride 是像素
            for (int yy = 0; yy < rows; yy++) {
                const uint8_t *s = src + (size_t)yy * stride;
                uint8_t *d = (uint8_t *)ab.bits + (size_t)yy * dst_row;
                if (fmt == WL_SHM_FORMAT_ARGB8888) {
                    // 图层 BGRA_8888，ARGB8888 内存就是 B,G,R,A → 整行搬 + alpha 顶成 FF
                    const uint32_t *s32 = (const uint32_t *)s;
                    uint32_t *d32 = (uint32_t *)d;
                    for (int xx = 0; xx < cols; xx++) d32[xx] = s32[xx] | 0xFF000000u;
                } else if (fmt == WL_SHM_FORMAT_XRGB8888) {
                    // X 位不能当成 alpha 用，必须强制不透明
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
                printf("[perf] %s %dx%d 拷贝+提交 %u ms\n",
                       is_subsurface ? "子" : "主", sw, sh, now_ms() - __t0);
            if (g_in_drag && (frames % 5) == 1)
                printf("[move] 拖拽中客户端仍在重绘 frame=%d\n", frames);
            if ((++frames % 30) == 1)
                printf("[sfwm] '%s' frame %d  %dx%d stride=%d fmt=%d\n",
                       title[0] ? title : "view", frames, ab.width, ab.height, ab.stride, ab.format);
        } else {
            printf("[sfwm] lock 失败\n");
        }

        wl_buffer_send_release(buffer);
    }

    bool commit() {
        // 逃生开关：万一延后读出问题，一个环境变量退回旧行为（不收编帧）
        if (pending_buffer && getenv("SFWM_NO_DEFER")) {
            blit(pending_buffer);
            pending_buffer = nullptr;
            pres_send_presented(this);
        }
        if (pending_buffer) {
            // 读的是**上一帧**：它到达时我们又过了一个 commit 周期，
            // 客户端的 GPU 渲染铁定已经完成，不会再读到半成品。
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
        if (frame_cb) {
            wl_callback_send_done(frame_cb, now_ms());
            wl_resource_destroy(frame_cb);
            frame_cb = nullptr;
        }
        return true;
    }
};
