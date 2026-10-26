// sf_layer.hpp —— SurfaceFlinger 图层的最小封装（Android 16 实测 ABI）
// 依赖：/system/lib64/libgui.so, /system/lib64/libutils.so
// 关键点见 libgui/README.md：sp<T> 走 X8 sret；ANativeWindow 在 Surface+0x10
#pragma once

#include <android/native_window.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace sf {

struct Rect { int32_t left, top, right, bottom; };

// >16 字节的聚合体 → clang 才会用 X8 传 sret
struct Sret24 { void *a, *b, *c; };

typedef Sret24 (*Fn_getDefault)(void);
typedef Sret24 (*Fn_getSurface)(void *sc);
typedef void   (*Fn_createSurfaceChecked)(void);   // 只用地址，走手写 asm 调用
typedef void (*Fn_String8_ctor)(void *out, const char *s);
typedef void (*Fn_String8_dtor)(void *p);
typedef void (*Fn_LayerMetadata_ctor)(void *p);
typedef void (*Fn_Tx_ctor)(void *tx);
typedef void (*Fn_Tx_setGeometry)(void *tx, const void *sp_sc, const Rect *src, const Rect *dst, int32_t orient);
typedef void (*Fn_Tx_setPosition)(void *tx, const void *sp_sc, float x, float y);
typedef void (*Fn_Tx_setLayer)(void *tx, const void *sp_sc, int32_t z);
typedef void (*Fn_Tx_setAlpha)(void *tx, const void *sp_sc, float a);
typedef void (*Fn_Tx_show)(void *tx, const void *sp_sc);
typedef void (*Fn_Tx_hide)(void *tx, const void *sp_sc);
typedef void (*Fn_Tx_reparent)(void *tx, const void *sp_sc, const void *sp_parent);
typedef void (*Fn_Tx_setFlags)(void *tx, const void *sp_sc, uint32_t flags, uint32_t mask);    // Android 12+ 的输入遮挡：图层即使 touchableRegion 为空，只要 touchOcclusionMode
    // 是 BLOCK_UNTRUSTED 且像素不透明度够高，就会把下面「不受信任」窗口的触摸吞掉。
    // 光标这种纯装饰覆盖层必须显式声明：受信任覆盖层 + 丢弃所有输入。
    typedef void (*Fn_Tx_setTrustedOverlay)(void *tx, const void *sp_sc, int trusted);
    typedef void (*Fn_Tx_setDropInputMode)(void *tx, const void *sp_sc, int mode);
    typedef int32_t (*Fn_Tx_apply)(void *tx, bool sync, bool oneWay);

// layer_state_t 的关键位（frameworks/native/include/gui/LayerState.h）
static constexpr uint32_t SF_LAYER_HIDDEN        = 0x01;
static constexpr uint32_t SF_LAYER_OPAQUE        = 0x02;
static constexpr uint32_t SF_LAYER_NOT_FOCUSABLE = 0x08;
static constexpr uint32_t SF_LAYER_NOT_TOUCHABLE = 0x10;

// ---- ABI 桥 ----
// getDefault() 返回 sp<SurfaceComposerClient>，是"非平凡返回值" → 走 X8 (sret)。
// 直接用 C 函数指针按 x0 传会写到野指针上，必须用 naked 桥把 x8 指过去。
__attribute__((naked, noinline))
static void get_default_bridge(void *, void *) {
    asm volatile(
        "stp x30, xzr, [sp, #-16]!\n"
        "mov x8, x0\n"
        "blr x1\n"
        "ldp x30, xzr, [sp], #16\n"
        "ret\n");
}

// getSurface() 同理，sp<Surface> 也是 X8 返回
__attribute__((naked, noinline))
static void get_surface_bridge(void *, void *, void *) {
    asm volatile(
        "stp x30, xzr, [sp, #-16]!\n"
        "mov x8, x2\n"
        "blr x1\n"
        "ldp x30, xzr, [sp], #16\n"
        "ret\n");
}

// createSurfaceChecked 的 9 个参数：x1..x7 + 栈上 [sp]=&metadata, [sp+8]=outTransformHint
static inline int32_t call_create_surface_checked(
    void *fn, void *client, const void *name8,
    uint32_t w, uint32_t h, int32_t format,
    void **out_sc, int32_t flags, const void *parent_sp,
    const void *metadata, uint32_t *out_hint) {
    struct Extra { const void *meta; uint32_t *hint; } extra{metadata, out_hint};
    int32_t ret;
    register void *fn_reg asm("x16") = fn;
    asm volatile(
        "ldp x9, x10, [%[extra]]\n"
        "sub sp, sp, #16\n"
        "stp x9, x10, [sp]\n"
        "mov x0, %[client]\n"
        "mov x1, %[name]\n"
        "mov w2, %w[w]\n"
        "mov w3, %w[h]\n"
        "mov w4, %w[format]\n"
        "mov x5, %[out_sc]\n"
        "mov w6, %w[flags]\n"
        "mov x7, %[parent]\n"
        "blr x16\n"
        "add sp, sp, #16\n"
        "mov %w[ret], w0\n"
        : [ret] "=r"(ret)
        : [fn_reg] "r"(fn_reg), [client] "r"(client), [name] "r"(name8),
          [w] "r"(w), [h] "r"(h), [format] "r"(format),
          [out_sc] "r"(out_sc), [flags] "r"(flags),
          [parent] "r"(parent_sp), [extra] "r"(&extra)
        : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7",
          "x8", "x9", "x10", "x30", "memory", "cc");
    return ret;
}

struct Lib {
    void *libgui = nullptr;
    void *libutils = nullptr;
    void *client = nullptr;          // SurfaceComposerClient*
    Fn_getDefault            getDefault = nullptr;
    Fn_getSurface            getSurface = nullptr;
    Fn_createSurfaceChecked  createSurfaceChecked = nullptr;
    Fn_String8_ctor         s8_ctor = nullptr;
    Fn_String8_dtor         s8_dtor = nullptr;
    Fn_LayerMetadata_ctor   md_ctor = nullptr;
    Fn_Tx_ctor              tx_ctor = nullptr;
    Fn_Tx_setGeometry       tx_setGeometry = nullptr;
    Fn_Tx_setPosition       tx_setPosition = nullptr;
    Fn_Tx_setLayer          tx_setLayer = nullptr;
    Fn_Tx_setAlpha          tx_setAlpha = nullptr;
    Fn_Tx_show              tx_show = nullptr;
    Fn_Tx_hide              tx_hide = nullptr;
    Fn_Tx_reparent          tx_reparent = nullptr;
    Fn_Tx_setFlags          tx_setFlags = nullptr;
    Fn_Tx_setTrustedOverlay tx_setTrustedOverlay = nullptr;
    Fn_Tx_setDropInputMode  tx_setDropInputMode = nullptr;
    Fn_Tx_apply             tx_apply = nullptr;
    bool ok = false;

    static Lib &get() { static Lib g; static bool once = false; if (!once) { once = true; g.init(); } return g; }

    static void *sym(void *h, const char *n, bool required) {
        void *p = dlsym(h, n);
        if (!p && required) { fprintf(stderr, "[sf] 缺符号 %s\n", n); abort(); }
        return p;
    }

    void init() {
        libgui = dlopen("/system/lib64/libgui.so", RTLD_NOW | RTLD_GLOBAL);
        if (!libgui) libgui = dlopen("libgui.so", RTLD_NOW | RTLD_GLOBAL);
        if (!libgui) { fprintf(stderr, "[sf] libgui: %s\n", dlerror()); return; }
        libutils = dlopen("/system/lib64/libutils.so", RTLD_NOW | RTLD_GLOBAL);
        if (!libutils) libutils = dlopen("libutils.so", RTLD_NOW | RTLD_GLOBAL);
        if (!libutils) { fprintf(stderr, "[sf] libutils: %s\n", dlerror()); return; }

        getDefault    = (Fn_getDefault)   sym(libgui, "_ZN7android21SurfaceComposerClient10getDefaultEv", true);
        getSurface    = (Fn_getSurface)   sym(libgui, "_ZN7android14SurfaceControl10getSurfaceEv", true);
        createSurfaceChecked = (Fn_createSurfaceChecked) sym(libgui,
            "_ZN7android21SurfaceComposerClient20createSurfaceCheckedERKNS_7String8Ejji"
            "PNS_2spINS_14SurfaceControlEEEiRKNS4_INS_7IBinderEEENS_3gui13LayerMetadataEPj", true);
        md_ctor        = (Fn_LayerMetadata_ctor) sym(libgui, "_ZN7android3gui13LayerMetadataC1Ev", true);
        tx_ctor        = (Fn_Tx_ctor)       sym(libgui, "_ZN7android21SurfaceComposerClient11TransactionC1Ev", true);
        tx_setGeometry = (Fn_Tx_setGeometry)sym(libgui, "_ZN7android21SurfaceComposerClient11Transaction11setGeometryERKNS_2spINS_14SurfaceControlEEERKNS_4RectES9_i", true);
        tx_setPosition = (Fn_Tx_setPosition)sym(libgui, "_ZN7android21SurfaceComposerClient11Transaction11setPositionERKNS_2spINS_14SurfaceControlEEEff", true);
        tx_setLayer    = (Fn_Tx_setLayer)   sym(libgui, "_ZN7android21SurfaceComposerClient11Transaction8setLayerERKNS_2spINS_14SurfaceControlEEEi", true);
        tx_setAlpha    = (Fn_Tx_setAlpha)   sym(libgui, "_ZN7android21SurfaceComposerClient11Transaction8setAlphaERKNS_2spINS_14SurfaceControlEEEf", false);
        tx_show        = (Fn_Tx_show)       sym(libgui, "_ZN7android21SurfaceComposerClient11Transaction4showERKNS_2spINS_14SurfaceControlEEE", true);
        tx_hide        = (Fn_Tx_hide)       sym(libgui, "_ZN7android21SurfaceComposerClient11Transaction4hideERKNS_2spINS_14SurfaceControlEEE", true);
        tx_reparent    = (Fn_Tx_reparent)   sym(libgui, "_ZN7android21SurfaceComposerClient11Transaction8reparentERKNS_2spINS_14SurfaceControlEEES6_", false);
        // surfaceflinger 的“点击穿透”开关：eLayerNotTouchable（不导出头文件，但符号在）
        tx_setFlags    = (Fn_Tx_setFlags)   sym(libgui, "_ZN7android21SurfaceComposerClient11Transaction8setFlagsERKNS_2spINS_14SurfaceControlEEEjj", false);
        if (!tx_setFlags) printf("[sf] 警告：没有 setFlags 符号，点击穿透不可用\n");
        // Android 12+ 的输入遮挡：图层 touchableRegion 为空也不够，
        // touchOcclusionMode=BLOCK_UNTRUSTED 时仍会吞掉下面「不受信任」窗口的触摸。
        tx_setTrustedOverlay = (Fn_Tx_setTrustedOverlay)sym(libgui,
            "_ZN7android21SurfaceComposerClient11Transaction17setTrustedOverlayERKNS_2spINS_14SurfaceControlEEENS_3gui14TrustedOverlayE", false);
        tx_setDropInputMode = (Fn_Tx_setDropInputMode)sym(libgui,
            "_ZN7android21SurfaceComposerClient11Transaction16setDropInputModeERKNS_2spINS_14SurfaceControlEEENS_3gui13DropInputModeE", false);
        printf("[sf] 覆盖层输入控制：trustedOverlay=%s dropInputMode=%s\n",
               tx_setTrustedOverlay ? "有" : "无", tx_setDropInputMode ? "有" : "无");
        tx_apply       = (Fn_Tx_apply)      sym(libgui, "_ZN7android21SurfaceComposerClient11Transaction5applyEbb", true);
        s8_ctor        = (Fn_String8_ctor)  sym(libutils, "_ZN7android7String8C1EPKc", true);
        s8_dtor        = (Fn_String8_dtor)  sym(libutils, "_ZN7android7String8D1Ev", true);

        client = nullptr;
        get_default_bridge(&client, reinterpret_cast<void *>(getDefault));
        if (!client) { fprintf(stderr, "[sf] getDefault 返回空\n"); return; }
        ok = true;
        printf("[sf] SurfaceComposerClient = %p\n", client);
        printf("[sf]   mStatus(+0xD0)     = %d\n", *(int32_t *)((char *)client + 0xD0));
        printf("[sf]   mComposerSvc(+0xD8)= %p\n", *(void **)((char *)client + 0xD8));
    }

    // Transaction 的析构没导出，只能按需 new 一块然后泄漏（窗口操作不频繁）
    void *newTx() { void *p = calloc(1, 0x200); tx_ctor(p); return p; }

    // 拖拽时每帧都要 didTransaction，重用一个（AOSP 自己也这么用：apply() 后可继续）
    // true 时所有 Layer 改动只塞进 sharedTx，不立即 apply，
    // 等 end_batch() 一次性提交 —— 否则一帧内多次 apply，
    // SF 会在中间状态合成，缩放/拖拽时逐帧闪。
    bool defer = false;

    void *sharedTx = nullptr;
    void *txOnce() { if (!sharedTx) sharedTx = newTx(); return sharedTx; }
};

static inline void begin_batch() { Lib::get().defer = true; }
static inline void end_batch() {
    Lib &L = Lib::get();
    if (!L.defer) return;
    L.defer = false;
    L.tx_apply(L.txOnce(), false, false);   // 一次事务，原子生效
}

class Layer {
public:
    void *sc = nullptr;        // SurfaceControl*
    void *surface = nullptr;   // Surface*
    ANativeWindow *win = nullptr;
    int32_t w = 0, h = 0;
    int32_t cap_w = 0, cap_h = 0;      // 已分配的缓冲尺寸（>= 可见尺寸，留余量）

    // format 用 HAL_PIXEL_FORMAT_*：1 = RGBA_8888（内存 R,G,B,A），
    //   5 = BGRA_8888（内存 B,G,R,A）。客户端 dmabuf/shm 基本都是 ARGB8888，
    //   也就是内存 B,G,R,A —— 直接用 5 建层就不必逐像素交换了。
    bool create(const char *name, int32_t width, int32_t height, int32_t format = 1) {
        Lib &L = Lib::get();
        if (!L.ok) return false;
        printf("[sf] create '%s' %dx%d ...\n", name, width, height);
        alignas(8)  uint8_t name8[0x20] = {0};
        L.s8_ctor(name8, name);
        alignas(16) uint8_t md[0x40] = {0};
        L.md_ctor(md);
        void *null_sp[1] = {nullptr};
        uint32_t hint = 0;
        int32_t rc = call_create_surface_checked(
            reinterpret_cast<void *>(L.createSurfaceChecked), L.client, name8,
            (uint32_t)width, (uint32_t)height, format,
            &sc, 0, null_sp, md, &hint);
        L.s8_dtor(name8);
        printf("[sf]   -> rc=%d sc=%p\n", rc, sc);
        if (rc != 0 || !sc) { fprintf(stderr, "[sf] createSurfaceChecked 失败 rc=%d\n", rc); return false; }
        w = width; h = height;
        surface = nullptr;
        get_surface_bridge(sc, reinterpret_cast<void *>(L.getSurface), &surface);
        if (!surface) { fprintf(stderr, "[sf] getSurface 失败\n"); return false; }
        win = (ANativeWindow *)((char *)surface + 0x10);   // ANativeWindow 子对象在 +0x10
        return true;
    }

    void place(int32_t x, int32_t y, int32_t z) {
        Lib &L = Lib::get();
        void *sp_sc[1] = { sc };
        void *tx = L.txOnce();
        if (L.tx_setLayer) L.tx_setLayer(tx, sp_sc, z);
        if (L.tx_setPosition) L.tx_setPosition(tx, sp_sc, (float)x, (float)y);
        if (L.tx_show) L.tx_show(tx, sp_sc);
        if (!L.defer) L.tx_apply(tx, false, false);
    }

    // 只改位置，不发 setLayer/show。拖拽时每帧叫 place() 会让图层多走一遍 show，
    // 位置和 buffer 的提交就不同步了 —— 屏幕上就是闪。
    void move(int32_t x, int32_t y) {
        Lib &L = Lib::get();
        void *sp_sc[1] = { sc };
        void *tx = L.txOnce();
        if (L.tx_setPosition) L.tx_setPosition(tx, sp_sc, (float)x, (float)y);
        if (!L.defer) L.tx_apply(tx, false, false);
    }

    void setGeometry(const Rect &src, const Rect &dst) {
        Lib &L = Lib::get();
        void *sp_sc[1] = { sc };
        void *tx = L.txOnce();
        L.tx_setGeometry(tx, sp_sc, &src, &dst, 0);
        if (L.tx_show) L.tx_show(tx, sp_sc);
        if (!L.defer) L.tx_apply(tx, false, false);
    }

    void hide() {
        Lib &L = Lib::get();
        void *sp_sc[1] = { sc };
        void *tx = L.txOnce();
        L.tx_hide(tx, sp_sc);
        if (!L.defer) L.tx_apply(tx, false, false);
    }

    // 点击穿透：置 eLayerNotTouchable 后 InputFlinger 会跳过这层，
    // 触摸落到下面的安卓界面。屏幕上照样看得见（这是“可见但不可摸”）。
    // 注意：状态没变就不再发事务 —— 每帧多一次 apply() 会让图层闪一下。
    bool m_touchable = true;
    bool m_opaque = false;
    // 声明这一层是**不透明**的：SurfaceFlinger 会忽略 buffer 的 alpha 通道。
    //   ★ 必须开：我们为了性能直接把客户端的 ARGB8888 原样 memcpy 下来，
    //     alpha 位是客户端 GL 渲染遗留的不确定值；不开这个标志时 SF 按
    //     PREMULTIPLIED 混合，每帧 alpha 不同 → 画面忽明忽暗（闪烁）。
    // 显式显示/隐藏这一层（不改 buffer，不影响客户端）
    void setVisible(bool on) {
        Lib &L = Lib::get();
        void *sp_sc[1] = { sc };
        void *tx = L.txOnce();
        if (on) { if (L.tx_show) L.tx_show(tx, sp_sc); }
        else    { if (L.tx_hide) L.tx_hide(tx, sp_sc); }
        if (!L.defer) L.tx_apply(tx, false, false);
    }

    bool setOpaque(bool on) {
        Lib &L = Lib::get();
        if (on == m_opaque) return true;
        if (!L.tx_setFlags) return false;
        void *sp_sc[1] = { sc };
        void *tx = L.txOnce();
        L.tx_setFlags(tx, sp_sc, on ? SF_LAYER_OPAQUE : 0u, SF_LAYER_OPAQUE);
        if (!L.defer) L.tx_apply(tx, false, false);
        m_opaque = on;
        return true;
    }

    bool setTouchable(bool on) {
        Lib &L = Lib::get();
        if (on == m_touchable) return true;
        if (!L.tx_setFlags) return false;
        void *sp_sc[1] = { sc };
        void *tx = L.txOnce();
        L.tx_setFlags(tx, sp_sc, on ? 0u : SF_LAYER_NOT_TOUCHABLE, SF_LAYER_NOT_TOUCHABLE);
        if (!L.defer) L.tx_apply(tx, false, false);
        m_touchable = on;
        return true;
    }

    // 让覆盖层完全不参与输入（光标专用）。
    // DropInputMode: 0=ALL 1=OBSCURED 2=NONE；TrustedOverlay: 0=TRUSTED 1=UNTRUSTED
    bool makeInputTransparent() {
        Lib &L = Lib::get();
        void *sp_sc[1] = { sc };
        void *tx = L.txOnce();
        bool did = false;
        if (L.tx_setDropInputMode) { L.tx_setDropInputMode(tx, sp_sc, 0); did = true; }
        if (L.tx_setTrustedOverlay) { L.tx_setTrustedOverlay(tx, sp_sc, 0); did = true; }
        if (did) if (!L.defer) L.tx_apply(tx, false, false);
        return did;
    }
};

} // namespace sf
