// SPDX-License-Identifier: AGPL-3.0-or-later
// The system's own pointer sprite, driven from the module.
//
// SurfaceFlinger only offers the hardware cursor plane to a layer that behaves like the
// input system's sprite: a small buffer that never changes size and only ever moves. That
// layer is made by InputFlinger's `SpriteController`, which lives in libinputservice.so, so
// the same controller is bound here with dlsym and driven directly. Nothing is hooked: the
// class is only ever constructed by this file, and the factory it takes returns no parent,
// which attaches the sprite to the root of the display, above every window.
//
// A sprite also stays out of input dispatch: it is created without an input channel and gets
// no input window handle, so it cannot occlude a touch. A client layer drawn through libgui
// cannot be made to do that: it is given a touchable region over its pixels even without a
// channel, and `setTrustedOverlay`/`setDropInputMode` are ignored for a caller that is not
// the window manager.
//
// How this file gets to run: libinputservice.so is not a public library, so only a process in
// the platform's linker namespace may open it. A hooked application process — and a plain
// one — gets a classloader namespace and is refused. So this library is preloaded with
// LD_PRELOAD into an `app_process` that the compositor starts as root: a preloaded library
// keeps the default namespace, and its constructor thread does the work from there.
//
// Why a virtual machine is involved at all: an icon is an `ABitmap`, and libhwui can only
// make one from a Java `android.graphics.Bitmap`. The bitmap is therefore built here through
// JNI, on a thread attached to the VM `app_process` created, filled with a rasterised arrow,
// and handed over as an `ABitmap`.
//
// ABI notes: `sp<T>` returned by value travels in x8, so a hand written cast crashes in the
// callee's store. Declaring a non-trivial stand-in struct makes the compiler emit the same
// convention the platform uses, including for the `std::function` the controller takes.
#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <functional>

#define LOG_TAG "sfwm-sprite"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace {

// Non-trivial stand-ins so the compiler uses the platform's (sret) convention.
struct SpLooper {
    void *p;
    SpLooper() : p(nullptr) {}
    explicit SpLooper(void *q) : p(q) {}
    SpLooper(const SpLooper &o) : p(o.p) {}
    ~SpLooper() {}
};
struct SpFC {
    void *p;
    SpFC() : p(nullptr) {}
    SpFC(const SpFC &o) : p(o.p) {}
    ~SpFC() {}
};
// android::ui::LogicalDisplayId is a trivial strong typedef over int32_t.
struct DispId { int32_t v; };
// SpriteIcon is { ABitmap* bitmap; }, 8 bytes, and an empty one skips setIcon entirely.
struct SpriteIconStandIn { void *bitmap; };
// AndroidBitmapInfo from <android/bitmap.h>; only what the fill needs.
struct AndroidBitmapInfo {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    int32_t format;
    uint32_t flags;
};

constexpr const char *PATH_INPUTSERVICE = "/system/lib64/libinputservice.so";
constexpr const char *PATH_UTILS = "/system/lib64/libutils.so";
constexpr const char *PATH_HWUI = "/system/lib64/libhwui.so";
constexpr const char *PATH_JNIGRAPHICS = "/system/lib64/libjnigraphics.so";
constexpr const char *PATH_NATIVEHELPER = "/apex/com.android.art/lib64/libnativehelper.so";
constexpr const char *PATH_NATIVEHELPER_SYSTEM = "/system/lib64/libnativehelper.so";

constexpr const char *SYM_SPRITE_CTOR =
    "_ZN7android16SpriteControllerC1ERKNS_2spINS_6LooperEEEiNSt3__18functionIFNS1_INS_14"
    "SurfaceControlEEENS_2ui16LogicalDisplayIdEEEE";
constexpr const char *SYM_CREATE_SPRITE = "_ZN7android16SpriteController12createSpriteEv";
constexpr const char *SYM_UPDATE_SPRITES = "_ZN7android16SpriteController15doUpdateSpritesEv";
constexpr const char *SYM_SET_ICON =
    "_ZN7android16SpriteController10SpriteImpl7setIconERKNS_10SpriteIconE";
constexpr const char *SYM_SET_DISPLAY =
    "_ZN7android16SpriteController10SpriteImpl12setDisplayIdENS_2ui16LogicalDisplayIdE";
constexpr const char *SYM_SET_POSITION = "_ZN7android16SpriteController10SpriteImpl11setPositionEff";
constexpr const char *SYM_SET_VISIBLE = "_ZN7android16SpriteController10SpriteImpl10setVisibleEb";
constexpr const char *SYM_SET_ALPHA = "_ZN7android16SpriteController10SpriteImpl8setAlphaEf";
constexpr const char *SYM_SET_LAYER = "_ZN7android16SpriteController10SpriteImpl8setLayerEi";
constexpr const char *SYM_LOOPER_PREPARE = "_ZN7android6Looper7prepareEi";
constexpr const char *SYM_ABITMAP_FROM_JAVA = "ABitmap_acquireBitmapFromJava";

constexpr int32_t ICON_SIZE = 84;   // the system's own sprite on this panel
constexpr int32_t LAYER = 681001;   // above every window, as the input service's sprites
constexpr int32_t DISPLAY = 0;

void *openLibrary(const char *path) {
    void *handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!handle) LOGW("cannot open %s: %s", path, dlerror());
    return handle;
}

struct Host {
    void *controller = nullptr;
    void *looper = nullptr;
    void *sprite = nullptr;
    bool visible = false;
    int32_t lastX = -1, lastY = -1;

    SpLooper (*prepare)(int) = nullptr;
    void (*ctor)(void *, const SpLooper &, int32_t, std::function<SpFC(DispId)>) = nullptr;
    SpFC (*createSprite)(void *) = nullptr;
    void (*updateSprites)(void *) = nullptr;
    void (*setIcon)(void *, const SpriteIconStandIn &) = nullptr;
    void (*setDisplayId)(void *, DispId) = nullptr;
    void (*setPosition)(void *, float, float) = nullptr;
    void (*setVisible)(void *, bool) = nullptr;
    void (*setAlpha)(void *, float) = nullptr;
    void (*setLayer)(void *, int32_t) = nullptr;
    void *(*bitmapFromJava)(JNIEnv *, jobject) = nullptr;
    int32_t (*bitmapInfo)(JNIEnv *, jobject, AndroidBitmapInfo *) = nullptr;
    int32_t (*lockPixels)(JNIEnv *, jobject, void **) = nullptr;
    int32_t (*unlockPixels)(JNIEnv *, jobject) = nullptr;
};

Host g;

// The controller queues surface changes and they only reach SurfaceFlinger when it is
// pumped, which is what `SpriteController::doUpdateSprites` does; the looper callback inside
// the input service calls it on every vsync. Driving it directly keeps this process free of
// a display event receiver and of the pointer animations that come with one.
void pump() {
    if (g.controller && g.updateSprites) g.updateSprites(g.controller);
}

bool resolveSprite() {
    void *input = openLibrary(PATH_INPUTSERVICE);
    void *utils = openLibrary(PATH_UTILS);
    if (!input || !utils) return false;
    // libhwui is already loaded by the virtual machine, and libandroid_runtime exports the
    // same symbol, so a null handle still resolves through the default scope.
    void *hwui = openLibrary(PATH_HWUI);

    g.prepare = (SpLooper(*)(int))dlsym(utils, SYM_LOOPER_PREPARE);
    g.ctor = (void (*)(void *, const SpLooper &, int32_t, std::function<SpFC(DispId)>))
        dlsym(input, SYM_SPRITE_CTOR);
    g.createSprite = (SpFC(*)(void *))dlsym(input, SYM_CREATE_SPRITE);
    g.updateSprites = (void (*)(void *))dlsym(input, SYM_UPDATE_SPRITES);
    g.setIcon = (void (*)(void *, const SpriteIconStandIn &))dlsym(input, SYM_SET_ICON);
    g.setDisplayId = (void (*)(void *, DispId))dlsym(input, SYM_SET_DISPLAY);
    g.setPosition = (void (*)(void *, float, float))dlsym(input, SYM_SET_POSITION);
    g.setVisible = (void (*)(void *, bool))dlsym(input, SYM_SET_VISIBLE);
    g.setAlpha = (void (*)(void *, float))dlsym(input, SYM_SET_ALPHA);
    g.setLayer = (void (*)(void *, int32_t))dlsym(input, SYM_SET_LAYER);
    // The symbol carries the LIBHWUI version, so the default version is not necessarily the
    // one dlsym looks for.
    const void *hwuiOrDefault = hwui ? hwui : RTLD_DEFAULT;
    g.bitmapFromJava = (void *(*)(JNIEnv *, jobject))
        dlvsym(const_cast<void *>(hwuiOrDefault), SYM_ABITMAP_FROM_JAVA, "LIBHWUI");
    if (!g.bitmapFromJava)
        g.bitmapFromJava =
            (void *(*)(JNIEnv *, jobject))dlsym(const_cast<void *>(hwuiOrDefault),
                                                SYM_ABITMAP_FROM_JAVA);
    if (!g.prepare || !g.ctor || !g.createSprite || !g.updateSprites ||
        !g.setIcon || !g.setDisplayId || !g.setPosition || !g.setVisible || !g.setAlpha ||
        !g.bitmapFromJava) {
        LOGW("the sprite interface is missing a symbol, the cursor stays off");
        return false;
    }
    return true;
}

// The pixel accessors of a Java bitmap; libjnigraphics is what every application uses too.
bool resolveBitmap() {
    void *graphics = openLibrary(PATH_JNIGRAPHICS);
    if (!graphics) return false;
    g.bitmapInfo = (int32_t(*)(JNIEnv *, jobject, AndroidBitmapInfo *))
        dlsym(graphics, "AndroidBitmap_getInfo");
    g.lockPixels = (int32_t(*)(JNIEnv *, jobject, void **))
        dlsym(graphics, "AndroidBitmap_lockPixels");
    g.unlockPixels = (int32_t(*)(JNIEnv *, jobject))
        dlsym(graphics, "AndroidBitmap_unlockPixels");
    return g.bitmapInfo && g.lockPixels && g.unlockPixels;
}

// The virtual machine belongs to app_process and is created after this library is preloaded,
// so both the symbol and the VM are waited for: at preload time ART has not necessarily been
// loaded yet, and a second copy of libnativehelper opened here would keep its own idea of
// which VMs exist.
JavaVM *waitForVm(int seconds) {
    auto getVms = (jint(*)(JavaVM **, jsize, jsize *))nullptr;
    for (int i = 0; i < seconds * 20; i++) {
        if (!getVms) {
            getVms = (jint(*)(JavaVM **, jsize, jsize *))
                dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs");
            if (!getVms) {
                void *helper = openLibrary(PATH_NATIVEHELPER);
                if (!helper) helper = openLibrary(PATH_NATIVEHELPER_SYSTEM);
                if (helper)
                    getVms = (jint(*)(JavaVM **, jsize, jsize *))
                        dlsym(helper, "JNI_GetCreatedJavaVMs");
            }
        }
        if (getVms) {
            JavaVM *vm = nullptr;
            jsize count = 0;
            if (getVms(&vm, 1, &count) == JNI_OK && count >= 1 && vm) return vm;
        }
        usleep(50 * 1000);
    }
    LOGW("no virtual machine after %d s (symbol %p)", seconds, (void *)getVms);
    return nullptr;
}

// ------------------------------------------------------------------ the icon
// An arrow of the pointer-icon shape: black outline, white fill, tip at the top left. The
// picture is drawn once, at the size the sprite is created with, and never changes again.
void fillPolygon(uint8_t *base, int32_t width, int32_t height, int32_t stride,
                 const float *pts, int count, uint8_t r, uint8_t g8, uint8_t b) {
    float xs[16];
    for (int32_t y = 0; y < height; y++) {
        const float row = (float)y + 0.5f;
        int m = 0;
        for (int i = 0; i < count; i++) {
            const int j = (i + 1) % count;
            const float y0 = pts[2 * i + 1], y1 = pts[2 * j + 1];
            if (y0 == y1) continue;
            if ((row >= y0 && row < y1) || (row >= y1 && row < y0)) {
                const float t = (row - y0) / (y1 - y0);
                if (m < 16) xs[m++] = pts[2 * i] + t * (pts[2 * j] - pts[2 * i]);
            }
        }
        if (m < 2) continue;
        for (int a = 0; a < m; a++)
            for (int c = a + 1; c < m; c++)
                if (xs[c] < xs[a]) { const float t = xs[a]; xs[a] = xs[c]; xs[c] = t; }
        for (int a = 0; a + 1 < m; a += 2) {
            int32_t x0 = (int32_t)(xs[a] + 0.5f);
            int32_t x1 = (int32_t)(xs[a + 1] + 0.5f);
            if (x0 < 0) x0 = 0;
            if (x1 > width - 1) x1 = width - 1;
            for (int32_t x = x0; x <= x1; x++) {
                uint8_t *d = base + (size_t)y * (size_t)stride + (size_t)x * 4;
                d[0] = r;
                d[1] = g8;
                d[2] = b;
                d[3] = 0xFF;
            }
        }
    }
}

void drawArrow(uint8_t *base, int32_t width, int32_t height, int32_t stride) {
    const float unit = (float)width / 24.0f;
    const float shape[] = {
        1.0f, 1.0f, 1.0f, 18.6f, 5.2f, 14.0f, 8.2f, 21.2f,
        11.5f, 19.8f, 8.5f, 12.8f, 14.6f, 12.8f,
    };
    float outer[14];
    float cx = 0, cy = 0;
    for (int i = 0; i < 7; i++) {
        outer[2 * i] = shape[2 * i] * unit;
        outer[2 * i + 1] = shape[2 * i + 1] * unit;
        cx += outer[2 * i];
        cy += outer[2 * i + 1];
    }
    cx /= 7.0f;
    cy /= 7.0f;
    // The fill is the same shape pulled towards its centre, which leaves the outline.
    float inner[14];
    for (int i = 0; i < 7; i++) {
        const float dx = outer[2 * i] - cx, dy = outer[2 * i + 1] - cy;
        const float len = sqrtf(dx * dx + dy * dy);
        const float k = len > 1.6f ? (len - 1.2f) / len : 0.0f;
        inner[2 * i] = cx + dx * k;
        inner[2 * i + 1] = cy + dy * k;
    }
    fillPolygon(base, width, height, stride, outer, 7, 0, 0, 0);
    fillPolygon(base, width, height, stride, inner, 7, 0xFF, 0xFF, 0xFF);
}

jobject createIcon(JNIEnv *env) {
    jclass bitmapCls = env->FindClass("android/graphics/Bitmap");
    if (env->ExceptionCheck() || !bitmapCls) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        LOGW("android.graphics.Bitmap is not reachable");
        return nullptr;
    }
    jclass configCls = env->FindClass("android/graphics/Bitmap$Config");
    if (env->ExceptionCheck() || !configCls) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        LOGW("Bitmap$Config is not reachable");
        return nullptr;
    }
    jfieldID argb =
        env->GetStaticFieldID(configCls, "ARGB_8888", "Landroid/graphics/Bitmap$Config;");
    if (env->ExceptionCheck() || !argb) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        LOGW("Bitmap$Config.ARGB_8888 is not reachable");
        return nullptr;
    }
    jmethodID create = env->GetStaticMethodID(
        bitmapCls, "createBitmap", "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;");
    if (env->ExceptionCheck() || !create) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        LOGW("Bitmap.createBitmap is not reachable");
        return nullptr;
    }
    jobject config = env->GetStaticObjectField(configCls, argb);
    jobject bitmap = env->CallStaticObjectMethod(
        bitmapCls, create, (jint)ICON_SIZE, (jint)ICON_SIZE, config);
    if (env->ExceptionCheck() || !bitmap) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        LOGW("the icon bitmap was refused");
        return nullptr;
    }
    AndroidBitmapInfo info = {};
    void *pixels = nullptr;
    if (g.bitmapInfo(env, bitmap, &info) == 0 && g.lockPixels(env, bitmap, &pixels) == 0 &&
        pixels) {
        memset(pixels, 0, (size_t)info.stride * info.height);
        drawArrow((uint8_t *)pixels, (int32_t)info.width, (int32_t)info.height,
                  (int32_t)info.stride);
        g.unlockPixels(env, bitmap);
    } else {
        LOGW("the icon bitmap has no pixels to draw into");
    }
    return bitmap;
}

bool startSprite(JNIEnv *env) {
    if (!resolveSprite() || !resolveBitmap()) return false;
    g.looper = g.prepare(0).p;
    if (!g.looper) {
        LOGW("this thread has no looper of its own");
        return false;
    }
    // The storage is never freed: the controller lives as long as the process.
    g.controller = calloc(1, 512);
    if (!g.controller) return false;
    // No parent: the sprite becomes a child of the display root, which is what puts it above
    // every window and keeps it eligible for the cursor plane.
    g.ctor(g.controller, SpLooper(g.looper), 0, [](DispId) { return SpFC(); });

    jobject icon = createIcon(env);
    if (!icon) return false;
    g.sprite = g.createSprite(g.controller).p;
    if (!g.sprite) {
        LOGW("createSprite returned nothing");
        return false;
    }
    void *bitmap = g.bitmapFromJava(env, icon);
    if (!bitmap) {
        LOGW("the icon was refused as an ABitmap");
        return false;
    }
    SpriteIconStandIn iconRef{bitmap};
    g.setIcon(g.sprite, iconRef);
    g.setAlpha(g.sprite, 1.0f);
    if (g.setLayer) g.setLayer(g.sprite, LAYER);
    pump();
    LOGI("sprite %p ready, looper %p", g.sprite, g.looper);
    return true;
}

// ------------------------------------------------------------------ commands
void handleCommand(const char *phase, int32_t x, int32_t y) {
    if (!g.sprite) return;
    if (strcmp(phase, "hide") == 0) {
        if (!g.visible) return;
        g.setVisible(g.sprite, false);
        g.visible = false;
        pump();
        return;
    }
    // The hotspot is the arrow's tip, one unit inside the icon.
    const float hotspot = (float)ICON_SIZE / 24.0f;
    if (g.visible && strcmp(phase, "move") == 0) {
        if (x == g.lastX && y == g.lastY) return;
        g.setPosition(g.sprite, (float)x - hotspot, (float)y - hotspot);
    } else {
        g.setDisplayId(g.sprite, DispId{DISPLAY});
        g.setPosition(g.sprite, (float)x - hotspot, (float)y - hotspot);
        g.setVisible(g.sprite, true);
        g.visible = true;
        g.lastX = x;
        g.lastY = y;
        // Appearing starts a fade in the sprite's own state; a pointer has to be solid, so
        // the alpha is put back after the visibility change rather than before it.
        g.setAlpha(g.sprite, 1.0f);
        pump();
        return;
    }
    g.lastX = x;
    g.lastY = y;
    pump();
}

void run() {
    JavaVM *vm = waitForVm(10);
    if (!vm) return;
    // The runtime reports itself through JNI_GetCreatedJavaVMs while it is still starting, and
    // attaching that early faults inside art::Thread::CreatePeer. One second of grace is
    // enough for the main thread group to exist.
    sleep(2);
    JNIEnv *env = nullptr;
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) {
        LOGW("cannot attach to the virtual machine");
        return;
    }
    LOGI("attached to the virtual machine, env %p", env);
    if (!startSprite(env)) return;

    char line[128];
    while (fgets(line, sizeof line, stdin)) {
        char phase[16] = {0};
        int x = 0, y = 0;
        const int n = sscanf(line, "%15s %d %d", phase, &x, &y);
        if (n >= 1) handleCommand(phase, x, y);
    }
    // End of input: the compositor is going away, and the picture goes with it. The virtual
    // machine would keep this process alive on its own thread, so it is left from here.
    handleCommand("hide", 0, 0);
    LOGI("input closed, host done");
    _exit(0);
}

// A preloaded library keeps the platform's linker namespace, which is the whole reason the
// work happens here and not in the process that would have loaded it with System.load.
__attribute__((constructor)) void bootstrap() {
    pthread_t thread;
    if (pthread_create(&thread, nullptr, [](void *) -> void * { run(); return nullptr; },
                       nullptr) == 0) {
        pthread_detach(thread);
    }
}

}  // namespace
