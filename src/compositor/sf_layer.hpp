// SPDX-License-Identifier: AGPL-3.0-or-later
// sf_layer.hpp - minimal SurfaceFlinger layer wrapper (ABI as measured on Android 16).
// Depends on /system/lib64/libgui.so and /system/lib64/libutils.so.
// sp<T> returns travel in X8 (sret); the ANativeWindow lives at Surface+0x10.
#pragma once

#include <android/native_window.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace sf {

struct Rect { int32_t left, top, right, bottom; };

// Aggregates larger than 16 bytes make clang return through X8 (sret).
struct Sret24 { void *a, *b, *c; };

typedef Sret24 (*Fn_getDefault)(void);
typedef Sret24 (*Fn_getSurface)(void *sc);
typedef void   (*Fn_createSurfaceChecked)(void);   // address only, called through a handwritten asm bridge
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
typedef void (*Fn_Tx_setFlags)(void *tx, const void *sp_sc, uint32_t flags, uint32_t mask);    // Android 12+ input occlusion: an empty touchableRegion does not stop a layer with
    // touchOcclusionMode BLOCK_UNTRUSTED and opaque pixels from swallowing touches that
    // target untrusted windows below. Decorative overlays must be trusted and drop input.
    typedef void (*Fn_Tx_setTrustedOverlay)(void *tx, const void *sp_sc, int trusted);
    typedef void (*Fn_Tx_setDropInputMode)(void *tx, const void *sp_sc, int mode);
    typedef int32_t (*Fn_Tx_apply)(void *tx, bool sync, bool oneWay);

// Selected bits of layer_state_t (frameworks/native/include/gui/LayerState.h).
static constexpr uint32_t SF_LAYER_HIDDEN        = 0x01;
static constexpr uint32_t SF_LAYER_OPAQUE        = 0x02;
static constexpr uint32_t SF_LAYER_NOT_FOCUSABLE = 0x08;
static constexpr uint32_t SF_LAYER_NOT_TOUCHABLE = 0x10;

// ---- ABI bridges ----
// getDefault() returns sp<SurfaceComposerClient>, a non-trivial value passed in X8.
// A plain C function pointer would write the result through a wild pointer, so a naked
__attribute__((naked, noinline))
static void get_default_bridge(void *, void *) {
    asm volatile(
        "stp x30, xzr, [sp, #-16]!\n"
        "mov x8, x0\n"
        "blr x1\n"
        "ldp x30, xzr, [sp], #16\n"
        "ret\n");
}

// getSurface() likewise returns sp<Surface> through X8.
__attribute__((naked, noinline))
static void get_surface_bridge(void *, void *, void *) {
    asm volatile(
        "stp x30, xzr, [sp, #-16]!\n"
        "mov x8, x2\n"
        "blr x1\n"
        "ldp x30, xzr, [sp], #16\n"
        "ret\n");
}

// createSurfaceChecked takes nine arguments: x1..x7, plus [sp] = &metadata and [sp+8] = outTransformHint.
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
        if (!p && required) { fprintf(stderr, "[sf] missing symbol %s\n", n); abort(); }
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
        // Click-through flag: eLayerNotTouchable, not in any public header but exported.
        tx_setFlags    = (Fn_Tx_setFlags)   sym(libgui, "_ZN7android21SurfaceComposerClient11Transaction8setFlagsERKNS_2spINS_14SurfaceControlEEEjj", false);
        if (!tx_setFlags) printf("[sf] warning: setFlags is missing, click-through unavailable\n");
        // Android 12+ input occlusion: an empty touchableRegion is not enough when
        // touchOcclusionMode is BLOCK_UNTRUSTED; the layer still swallows those touches.
        tx_setTrustedOverlay = (Fn_Tx_setTrustedOverlay)sym(libgui,
            "_ZN7android21SurfaceComposerClient11Transaction17setTrustedOverlayERKNS_2spINS_14SurfaceControlEEENS_3gui14TrustedOverlayE", false);
        tx_setDropInputMode = (Fn_Tx_setDropInputMode)sym(libgui,
            "_ZN7android21SurfaceComposerClient11Transaction16setDropInputModeERKNS_2spINS_14SurfaceControlEEENS_3gui13DropInputModeE", false);
        printf("[sf] overlay input control: trustedOverlay=%s dropInputMode=%s\n",
               tx_setTrustedOverlay ? "yes" : "no", tx_setDropInputMode ? "yes" : "no");
        tx_apply       = (Fn_Tx_apply)      sym(libgui, "_ZN7android21SurfaceComposerClient11Transaction5applyEbb", true);
        s8_ctor        = (Fn_String8_ctor)  sym(libutils, "_ZN7android7String8C1EPKc", true);
        s8_dtor        = (Fn_String8_dtor)  sym(libutils, "_ZN7android7String8D1Ev", true);

        client = nullptr;
        get_default_bridge(&client, reinterpret_cast<void *>(getDefault));
        if (!client) { fprintf(stderr, "[sf] getDefault returned null\n"); return; }
        ok = true;
        printf("[sf] SurfaceComposerClient = %p\n", client);
        printf("[sf]   mStatus(+0xD0)     = %d\n", *(int32_t *)((char *)client + 0xD0));
        printf("[sf]   mComposerSvc(+0xD8)= %p\n", *(void **)((char *)client + 0xD8));
    }

    // The Transaction destructor is not exported, so each instance is deliberately leaked.
    void *newTx() { void *p = calloc(1, 0x200); tx_ctor(p); return p; }

    // Transactions are reused: apply() leaves them usable, as AOSP also relies on.
    // While defer is set, layer changes go into sharedTx and are not applied,
    // so that end_batch() submits them at once. Several applies per frame make
    // SurfaceFlinger composite intermediate states and flicker during a gesture.
    bool defer = false;

    void *sharedTx = nullptr;
    void *txOnce() { if (!sharedTx) sharedTx = newTx(); return sharedTx; }
};

static inline void begin_batch() { Lib::get().defer = true; }
static inline void end_batch() {
    Lib &L = Lib::get();
    if (!L.defer) return;
    L.defer = false;
    L.tx_apply(L.txOnce(), false, false);   // one transaction, applied atomically
}

class Layer {
public:
    void *sc = nullptr;        // SurfaceControl*
    void *surface = nullptr;   // Surface*
    ANativeWindow *win = nullptr;
    int32_t w = 0, h = 0;
    int32_t cap_w = 0, cap_h = 0;      // allocated buffer size, at least the visible size

    // format uses HAL_PIXEL_FORMAT_*: 1 = RGBA_8888 (R,G,B,A in memory),
    // 5 = BGRA_8888 (B,G,R,A). Client dmabufs and shm buffers are mostly ARGB8888,
    // that is B,G,R,A, so creating the layer with 5 avoids a per-pixel swap.
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
        if (rc != 0 || !sc) { fprintf(stderr, "[sf] createSurfaceChecked failed rc=%d\n", rc); return false; }
        w = width; h = height;
        surface = nullptr;
        get_surface_bridge(sc, reinterpret_cast<void *>(L.getSurface), &surface);
        if (!surface) { fprintf(stderr, "[sf] getSurface failed\n"); return false; }
        win = (ANativeWindow *)((char *)surface + 0x10);   // the ANativeWindow subobject is at +0x10
        return true;
    }

    void place(int32_t x, int32_t y, int32_t z) {
        Lib &L = Lib::get();
        // sc may not exist yet: a layer surface before configure, or a failed createSurfaceChecked.
        // Passing an empty SurfaceControl into a transaction makes libgui dereference
        // null in TransactionState::getLayerState(), which crashes the process.
        if (!sc) { fprintf(stderr, "[sf] no SF transaction: sc not created\n"); return; }
        void *sp_sc[1] = { sc };
        void *tx = L.txOnce();
        if (L.tx_setLayer) L.tx_setLayer(tx, sp_sc, z);
        if (L.tx_setPosition) L.tx_setPosition(tx, sp_sc, (float)x, (float)y);
        if (L.tx_show) L.tx_show(tx, sp_sc);
        if (!L.defer) L.tx_apply(tx, false, false);
    }

    // Move only. Calling place() every frame during a drag runs show again and
    // desynchronises the position from the buffer commit, which flickers.
    void move(int32_t x, int32_t y) {
        Lib &L = Lib::get();
        // sc may not exist yet: a layer surface before configure, or a failed createSurfaceChecked.
        // Passing an empty SurfaceControl into a transaction makes libgui dereference
        // null in TransactionState::getLayerState(), which crashes the process.
        if (!sc) { fprintf(stderr, "[sf] no SF transaction: sc not created\n"); return; }
        void *sp_sc[1] = { sc };
        void *tx = L.txOnce();
        if (L.tx_setPosition) L.tx_setPosition(tx, sp_sc, (float)x, (float)y);
        if (!L.defer) L.tx_apply(tx, false, false);
    }

    void setGeometry(const Rect &src, const Rect &dst) {
        Lib &L = Lib::get();
        // sc may not exist yet: a layer surface before configure, or a failed createSurfaceChecked.
        // Passing an empty SurfaceControl into a transaction makes libgui dereference
        // null in TransactionState::getLayerState(), which crashes the process.
        if (!sc) { fprintf(stderr, "[sf] no SF transaction: sc not created\n"); return; }
        void *sp_sc[1] = { sc };
        void *tx = L.txOnce();
        L.tx_setGeometry(tx, sp_sc, &src, &dst, 0);
        if (L.tx_show) L.tx_show(tx, sp_sc);
        if (!L.defer) L.tx_apply(tx, false, false);
    }

    void hide() {
        Lib &L = Lib::get();
        // sc may not exist yet: a layer surface before configure, or a failed createSurfaceChecked.
        // Passing an empty SurfaceControl into a transaction makes libgui dereference
        // null in TransactionState::getLayerState(), which crashes the process.
        if (!sc) { fprintf(stderr, "[sf] no SF transaction: sc not created\n"); return; }
        void *sp_sc[1] = { sc };
        void *tx = L.txOnce();
        L.tx_hide(tx, sp_sc);
        if (!L.defer) L.tx_apply(tx, false, false);
    }

    // Click-through: with eLayerNotTouchable set, InputFlinger skips this layer and the
    // touch reaches the Android UI below; the layer stays visible.
    // Unchanged state sends no transaction: an extra apply() per frame flickers.
    bool m_touchable = true;
    bool m_opaque = false;
    // Mark the layer opaque so SurfaceFlinger ignores the alpha channel of the buffer.
    // Client ARGB8888 pixels are copied as they are, so their alpha bytes carry
    // whatever GL left there. Without this flag SurfaceFlinger blends with
    // PREMULTIPLIED alpha and the picture brightens and dims from frame to frame.
    // Show or hide the layer without touching the buffer or the client.
    void setVisible(bool on) {
        Lib &L = Lib::get();
        // sc may not exist yet: a layer surface before configure, or a failed createSurfaceChecked.
        // Passing an empty SurfaceControl into a transaction makes libgui dereference
        // null in TransactionState::getLayerState(), which crashes the process.
        if (!sc) { fprintf(stderr, "[sf] no SF transaction: sc not created\n"); return; }
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
        // sc may not exist yet: a layer surface before configure, or a failed createSurfaceChecked.
        // Passing an empty SurfaceControl into a transaction makes libgui dereference
        // null in TransactionState::getLayerState(), which crashes the process.
        if (!sc) { fprintf(stderr, "[sf] no SF transaction: sc not created\n"); return false; }
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
        // sc may not exist yet: a layer surface before configure, or a failed createSurfaceChecked.
        // Passing an empty SurfaceControl into a transaction makes libgui dereference
        // null in TransactionState::getLayerState(), which crashes the process.
        if (!sc) { fprintf(stderr, "[sf] no SF transaction: sc not created\n"); return false; }
        void *sp_sc[1] = { sc };
        void *tx = L.txOnce();
        L.tx_setFlags(tx, sp_sc, on ? 0u : SF_LAYER_NOT_TOUCHABLE, SF_LAYER_NOT_TOUCHABLE);
        if (!L.defer) L.tx_apply(tx, false, false);
        m_touchable = on;
        return true;
    }

    // Keep an overlay out of input handling entirely (used by the cursor).
    // DropInputMode: 0=ALL 1=OBSCURED 2=NONE; TrustedOverlay: 0=TRUSTED 1=UNTRUSTED
    bool makeInputTransparent() {
        Lib &L = Lib::get();
        // sc may not exist yet: a layer surface before configure, or a failed createSurfaceChecked.
        // Passing an empty SurfaceControl into a transaction makes libgui dereference
        // null in TransactionState::getLayerState(), which crashes the process.
        if (!sc) { fprintf(stderr, "[sf] no SF transaction: sc not created\n"); return false; }
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
