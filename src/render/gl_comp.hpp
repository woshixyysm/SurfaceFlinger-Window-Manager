// SPDX-License-Identifier: AGPL-3.0-or-later
// gl_comp.hpp - composite client dmabufs on the GPU into the SurfaceFlinger layer buffer.
//
// Wayland compositors never read client dmabufs on the CPU. The buffer is imported as an
// EGLImage (EGL_LINUX_DMA_BUF_EXT), attached to a GL texture with
// glEGLImageTargetTexture2DOES and drawn into the compositor's own framebuffer. Three
// problems of the CPU copy disappear with it:
//   1. sync: sampling the texture makes the driver wait on the dma_resv fence, so no fence
//      guessing and no DMA_BUF_IOCTL_EXPORT_SYNC_FILE workaround is needed;
//   2. geometry: scaling and cropping become texture coordinates, and the client stride,
//      offset and modifier are handled by EGL instead of being applied by hand;
//   3. presentation: the render target is our own FBO, written to completion before it is
//      read back, so no pixel is left at alpha 0 for SurfaceFlinger to blend the desktop
//      through.
//
// One read-back remains. This Mesa build has no platform_android support, so there is no
// EGLSurface for an ANativeWindow and no eglSwapBuffers. The FBO is read with glReadPixels
// into the buffer returned by ANativeWindow_lock, with GL_PACK_ROW_LENGTH carrying the
// SurfaceFlinger stride. Rebuilding Mesa with platforms=android allows replacing that call
// with eglCreateWindowSurface plus eglSwapBuffers, without changes above this file.
//
// Measured on OnePlus PLQ110 (Adreno 830): GL 4.6 core profile, Mesa 26.2.5,
// EGL_EXT_image_dma_buf_import with modifiers, and gbm_bo_create(LINEAR) yielding
// modifier 0, which EGL imports directly.
//
// Set SFWM_GL=0 to fall back to the CPU copy.
#pragma once

#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace glc {

// ---- minimal EGL declarations, without depending on the EGL headers ----
typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLContext;
typedef void *EGLSurface;
typedef void *EGLImage;
typedef void *EGLClientBuffer;
typedef int32_t EGLint;

static const EGLDisplay EGL_NO_DISPLAY_ = nullptr;
static const EGLContext EGL_NO_CONTEXT_ = nullptr;
static const EGLSurface EGL_NO_SURFACE_ = nullptr;

static const EGLint EGL_NONE_          = 0x3038;
static const EGLint EGL_VENDOR_        = 0x3053;
static const EGLint EGL_VERSION_       = 0x3054;
static const EGLint EGL_EXTENSIONS_    = 0x3055;
static const EGLint EGL_HEIGHT_        = 0x3056;
static const EGLint EGL_WIDTH_         = 0x3057;
static const EGLint EGL_SURFACE_TYPE_  = 0x3033;
static const EGLint EGL_PBUFFER_BIT_   = 0x0001;
static const EGLint EGL_RENDERABLE_TYPE_ = 0x3040;
static const EGLint EGL_OPENGL_BIT_    = 0x0008;
static const EGLint EGL_RED_SIZE_      = 0x3024;
static const EGLint EGL_GREEN_SIZE_    = 0x3025;
static const EGLint EGL_BLUE_SIZE_     = 0x3026;
static const EGLint EGL_ALPHA_SIZE_    = 0x302B;
static const EGLint EGL_CONTEXT_MAJOR_VERSION_ = 0x3098;
static const EGLint EGL_CONTEXT_MINOR_VERSION_ = 0x30FB;
static const EGLint EGL_CONTEXT_OPENGL_PROFILE_MASK_ = 0x30FD;
static const EGLint EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_ = 0x00000001;
static const unsigned EGL_OPENGL_API_  = 0x30A2;
static const unsigned EGL_PLATFORM_SURFACELESS_MESA_ = 0x31DD;
static const unsigned EGL_LINUX_DMA_BUF_EXT_ = 0x3270;
static const EGLint EGL_LINUX_DRM_FOURCC_EXT_ = 0x3271;
static const EGLint EGL_DMA_BUF_PLANE0_FD_EXT_ = 0x3272;
static const EGLint EGL_DMA_BUF_PLANE0_OFFSET_EXT_ = 0x3273;
static const EGLint EGL_DMA_BUF_PLANE0_PITCH_EXT_ = 0x3274;
static const EGLint EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT_ = 0x3443;
static const EGLint EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT_ = 0x3444;
static const EGLint EGL_IMAGE_PRESERVED_KHR_ = 0x30D2;
static const EGLint EGL_TRUE_ = 1;

// ---- minimal GL declarations ----
static const unsigned GL_TEXTURE_2D_  = 0x0DE1;
static const unsigned GL_TEXTURE0_    = 0x84C0;
static const unsigned GL_RGBA8_       = 0x8058;
static const unsigned GL_RGBA_        = 0x1908;
static const unsigned GL_BGRA_        = 0x80E1;
static const unsigned GL_UNSIGNED_BYTE_ = 0x1401;
static const unsigned GL_FRAMEBUFFER_ = 0x8D40;
static const unsigned GL_COLOR_ATTACHMENT0_ = 0x8CE0;
static const unsigned GL_FRAMEBUFFER_COMPLETE_ = 0x8CD5;
static const unsigned GL_TEXTURE_MIN_FILTER_ = 0x2801;
static const unsigned GL_TEXTURE_MAG_FILTER_ = 0x2800;
static const unsigned GL_TEXTURE_WRAP_S_ = 0x2802;
static const unsigned GL_TEXTURE_WRAP_T_ = 0x2803;
static const unsigned GL_LINEAR_      = 0x2601;
static const unsigned GL_CLAMP_TO_EDGE_ = 0x812F;
static const unsigned GL_TRIANGLE_STRIP_ = 0x0005;
static const unsigned GL_COLOR_BUFFER_BIT_ = 0x4000;
static const unsigned GL_VERSION_     = 0x1F02;
static const unsigned GL_RENDERER_    = 0x1F01;
static const unsigned GL_PACK_ROW_LENGTH_ = 0x0D02;
static const unsigned GL_PACK_ALIGNMENT_ = 0x0D05;
static const unsigned GL_VERTEX_SHADER_ = 0x8B31;
static const unsigned GL_FRAGMENT_SHADER_ = 0x8B30;
static const unsigned GL_COMPILE_STATUS_ = 0x8B81;
static const unsigned GL_LINK_STATUS_ = 0x8B82;

// ---- entry point table ----
struct Api {
    void *h = nullptr;

    EGLDisplay (*GetPlatformDisplay)(unsigned, void *, const intptr_t *) = nullptr;
    int    (*Initialize)(EGLDisplay, int *, int *) = nullptr;
    const char *(*QueryString)(EGLDisplay, int) = nullptr;
    int    (*ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, int, int *) = nullptr;
    EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *) = nullptr;
    int    (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = nullptr;
    int    (*BindAPI)(unsigned) = nullptr;
    void *(*GetProcAddress)(const char *) = nullptr;
    EGLImage (*CreateImage)(EGLDisplay, EGLContext, unsigned, EGLClientBuffer,
                            const intptr_t *) = nullptr;
    int    (*DestroyImage)(EGLDisplay, EGLImage) = nullptr;

    void (*EGLImageTargetTexture2DOES)(unsigned, void *) = nullptr;
    void (*GenTextures)(int, unsigned *) = nullptr;
    void (*DeleteTextures)(int, const unsigned *) = nullptr;
    void (*BindTexture)(unsigned, unsigned) = nullptr;
    void (*TexParameteri)(unsigned, unsigned, int) = nullptr;
    void (*TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *) = nullptr;
    void (*GenFramebuffers)(int, unsigned *) = nullptr;
    void (*DeleteFramebuffers)(int, const unsigned *) = nullptr;
    void (*BindFramebuffer)(unsigned, unsigned) = nullptr;
    void (*FramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int) = nullptr;
    unsigned (*CheckFramebufferStatus)(unsigned) = nullptr;
    void (*Viewport)(int, int, int, int) = nullptr;
    void (*ClearColor)(float, float, float, float) = nullptr;
    void (*Clear)(unsigned) = nullptr;
    void (*ReadPixels)(int, int, int, int, unsigned, unsigned, void *) = nullptr;
    void (*PixelStorei)(unsigned, int) = nullptr;
    void (*DrawArrays)(unsigned, int, int) = nullptr;
    void (*GenVertexArrays)(int, unsigned *) = nullptr;
    void (*BindVertexArray)(unsigned) = nullptr;
    void (*UseProgram)(unsigned) = nullptr;
    void (*Uniform1i)(int, int) = nullptr;
    void (*Uniform1f)(int, float) = nullptr;
    unsigned (*CreateShader)(unsigned) = nullptr;
    void (*ShaderSource)(unsigned, int, const char *const *, const int *) = nullptr;
    void (*CompileShader)(unsigned) = nullptr;
    void (*GetShaderiv)(unsigned, unsigned, int *) = nullptr;
    void (*GetShaderInfoLog)(unsigned, int, int *, char *) = nullptr;
    unsigned (*CreateProgram)(void) = nullptr;
    void (*AttachShader)(unsigned, unsigned) = nullptr;
    void (*LinkProgram)(unsigned) = nullptr;
    void (*GetProgramiv)(unsigned, unsigned, int *) = nullptr;
    unsigned (*GetError)(void) = nullptr;
    const unsigned char *(*GetString)(unsigned) = nullptr;
};

static Api A;
static bool g_tried = false;
static bool g_ok = false;
static EGLDisplay g_dpy = EGL_NO_DISPLAY_;
static int g_uni_tex = -1, g_uni_flip = -1, g_uni_opaque = -1;
static unsigned g_vao = 0;
static unsigned g_prog = 0;
static float g_flip = -1.f;              // < 0 means take it from SFWM_GL_FLIP

// Framebuffer object, rebuilt whenever the size changes
static unsigned g_fbo = 0, g_ftex = 0;
static int g_fw = 0, g_fh = 0;
static int g_fbo_rebuild = 0;     // rebuild counter; a rebuild every frame means the size keeps changing

// Cache from client dmabuf to EGLImage and texture.
// The cache key includes the inode: a swapchain rebuild closes the old fd and the
// number is reused, so an fd-only key would keep reading a released buffer.
struct CacheEnt {
    int fd = -1;
    ino_t ino = 0;
    uint32_t w = 0, h = 0, fourcc = 0, stride = 0, offset = 0;
    uint64_t mod = 0;
    EGLImage img = nullptr;
    unsigned tex = 0;
};
static const int CACHE_N = 8;
static CacheEnt g_cache[CACHE_N];

static void *sym(Api &a, const char *n) {
    dlerror();
    void *p = dlsym(a.h, n);
    if (!p) p = a.GetProcAddress ? a.GetProcAddress(n) : nullptr;
    return p;
}

static bool shader_ok(unsigned s, Api &a, const char *what) {
    int ok = 0;
    a.GetShaderiv(s, GL_COMPILE_STATUS_, &ok);
    if (!ok) {
        char log[1024] = {0};
        a.GetShaderInfoLog(s, 1023, nullptr, log);
        printf("[gl] %s failed to compile %s: %s\n", what, log);
    }
    return ok != 0;
}

static bool init() {
    if (g_tried) return g_ok;
    g_tried = true;
    if (getenv("SFWM_GL") && !strcmp(getenv("SFWM_GL"), "0")) {
        printf("[gl] SFWM_GL=0, using the CPU copy\n");
        return g_ok = false;
    }
    // sfwm is started with `env -u LD_LIBRARY_PATH` so that the Termux libraries cannot
    // shadow bionic, so dlopen("libEGL.so.1") finds nothing. Absolute paths are tried instead.
    char home_buf[512];
    const char *home = getenv("HOME");
    if (home) snprintf(home_buf, sizeof(home_buf), "%s/lg/hw/libEGL.so.1", home);

    // Mesa's libEGL depends on libgallium, libexpat, libgbm, libdrm and libwayland-client,
    // all in $PREFIX/lib. That directory must never be added to the sfwm library path:
    // $PREFIX/lib/libEGL.so belongs to Mesa, and once it is visible, libgui.so resolves
    // against it and fails with
    // `cannot locate symbol "eglDestroySyncKHR" referenced by libgui.so`.
    // The dependencies are therefore loaded by absolute path with RTLD_GLOBAL first.
    {
        char pfx_buf[512];
        const char *pfx = getenv("PREFIX");
        if (!pfx && home) { snprintf(pfx_buf, sizeof(pfx_buf), "%s/../usr", home); pfx = pfx_buf; }
        if (pfx) {
            static const char *deps[] = {
                "libgallium-26.2.5.so", "libgallium.so", "libexpat.so", "libgbm.so",
                "libdrm.so", "libwayland-client.so", "libwayland-server.so",
            };
            for (unsigned i = 0; i < sizeof(deps) / sizeof(deps[0]); i++) {
                char p[768];
                snprintf(p, sizeof(p), "%s/lib/%s", pfx, deps[i]);
                dlopen(p, RTLD_NOW | RTLD_GLOBAL);      // failure is harmless, ld.so searches again
            }
        }
    }

    const char *cands[4] = {
        getenv("SFWM_LIBEGL"),
        home ? home_buf : nullptr,
        "libEGL.so.1",
        "libEGL.so",
    };
    for (int i = 0; i < 4 && !A.h; i++) {
        if (!cands[i] || !cands[i][0]) continue;
        A.h = dlopen(cands[i], RTLD_NOW | RTLD_GLOBAL);
        if (A.h) printf("[gl] libEGL = %s\n", cands[i]);
        else if (i < 2) printf("[gl] %s -> %s\n", cands[i], dlerror());
    }
    if (!A.h) { printf("[gl] cannot open libEGL: %s\n", dlerror()); return g_ok = false; }

    *(void **)(&A.GetPlatformDisplay) = dlsym(A.h, "eglGetPlatformDisplayEXT");
    if (!A.GetPlatformDisplay) *(void **)(&A.GetPlatformDisplay) = dlsym(A.h, "eglGetPlatformDisplay");
    *(void **)(&A.Initialize)     = dlsym(A.h, "eglInitialize");
    *(void **)(&A.QueryString)    = dlsym(A.h, "eglQueryString");
    *(void **)(&A.ChooseConfig)   = dlsym(A.h, "eglChooseConfig");
    *(void **)(&A.CreateContext)  = dlsym(A.h, "eglCreateContext");
    *(void **)(&A.MakeCurrent)    = dlsym(A.h, "eglMakeCurrent");
    *(void **)(&A.BindAPI)        = dlsym(A.h, "eglBindAPI");
    *(void **)(&A.GetProcAddress) = dlsym(A.h, "eglGetProcAddress");
    if (!A.GetPlatformDisplay || !A.Initialize || !A.GetProcAddress) {
        printf("[gl] libEGL is missing base symbols\n");
        return g_ok = false;
    }
    *(void **)(&A.CreateImage)  = sym(A, "eglCreateImageKHR");
    *(void **)(&A.DestroyImage) = sym(A, "eglDestroyImageKHR");
    if (!A.CreateImage) { printf("[gl] eglCreateImageKHR is missing\n"); return g_ok = false; }

    g_dpy = A.GetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA_, nullptr, nullptr);
    int maj = 0, min = 0;
    if (!g_dpy || !A.Initialize(g_dpy, &maj, &min)) {
        printf("[gl] surfaceless display initialisation failed\n");
        g_dpy = EGL_NO_DISPLAY_;
        return g_ok = false;
    }
    const char *dext = A.QueryString(g_dpy, EGL_EXTENSIONS_);
    if (!dext || !strstr(dext, "EGL_EXT_image_dma_buf_import")) {
        printf("[gl] EGL_EXT_image_dma_buf_import is missing, disabling the GPU path\n");
        return g_ok = false;
    }

    A.BindAPI(EGL_OPENGL_API_);
    static const EGLint cfgattr[] = {
        EGL_SURFACE_TYPE_, EGL_PBUFFER_BIT_,
        EGL_RENDERABLE_TYPE_, EGL_OPENGL_BIT_,
        EGL_RED_SIZE_, 8, EGL_GREEN_SIZE_, 8, EGL_BLUE_SIZE_, 8, EGL_ALPHA_SIZE_, 8,
        EGL_NONE_
    };
    EGLConfig cfg = nullptr;
    int nc = 0;
    if (!A.ChooseConfig(g_dpy, cfgattr, &cfg, 1, &nc) || nc < 1) {
        printf("[gl] eglChooseConfig failed\n");
        return g_ok = false;
    }
    EGLContext ctx = nullptr;
    static const EGLint ctx46[] = {
        EGL_CONTEXT_MAJOR_VERSION_, 4, EGL_CONTEXT_MINOR_VERSION_, 6,
        EGL_CONTEXT_OPENGL_PROFILE_MASK_, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_,
        EGL_NONE_
    };
    ctx = A.CreateContext(g_dpy, cfg, EGL_NO_CONTEXT_, ctx46);
    if (!ctx) {
        static const EGLint ctx33[] = {
            EGL_CONTEXT_MAJOR_VERSION_, 3, EGL_CONTEXT_MINOR_VERSION_, 3,
            EGL_CONTEXT_OPENGL_PROFILE_MASK_, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_,
            EGL_NONE_
        };
        ctx = A.CreateContext(g_dpy, cfg, EGL_NO_CONTEXT_, ctx33);
    }
    if (!ctx) { printf("[gl] eglCreateContext failed\n"); return g_ok = false; }
    if (!A.MakeCurrent(g_dpy, EGL_NO_SURFACE_, EGL_NO_SURFACE_, ctx)) {
        printf("[gl] eglMakeCurrent without a surface failed\n");
        return g_ok = false;
    }

// The "gl" prefix is required: eglGetProcAddress() expects glDrawArrays, not DrawArrays
#define L(f) *(void **)(&A.f) = sym(A, "gl" #f)
    L(EGLImageTargetTexture2DOES); L(GenTextures); L(DeleteTextures); L(BindTexture);
    L(TexParameteri); L(TexImage2D); L(GenFramebuffers); L(DeleteFramebuffers);
    L(BindFramebuffer); L(FramebufferTexture2D); L(CheckFramebufferStatus);
    L(Viewport); L(ClearColor); L(Clear); L(ReadPixels); L(PixelStorei);
    L(DrawArrays); L(GenVertexArrays); L(BindVertexArray); L(UseProgram);
    L(Uniform1i); L(Uniform1f); L(CreateShader); L(ShaderSource); L(CompileShader);
    L(GetShaderiv); L(GetShaderInfoLog); L(CreateProgram); L(AttachShader);
    L(LinkProgram); L(GetProgramiv); L(GetError);
#undef L
    *(void **)(&A.GetString) = sym(A, "glGetString");
    if (!A.EGLImageTargetTexture2DOES || !A.DrawArrays || !A.ReadPixels ||
        !A.CreateShader || !A.GetString) {
        printf("[gl] missing entry points (EGLImageTargetTexture2DOES, DrawArrays, ReadPixels)\n");
        return g_ok = false;
    }

    static const char *VS =
        "#version 330 core\n"
        "out vec2 v_uv;\n"
        "uniform float u_flip;\n"
        "void main() {\n"
        "  vec2 p = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));\n"
        "  v_uv = vec2(p.x, u_flip > 0.5 ? 1.0 - p.y : p.y);\n"
        "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";
    static const char *FS =
        "#version 330 core\n"
        "in vec2 v_uv;\n"
        "out vec4 o;\n"
        "uniform sampler2D u_tex;\n"
        "uniform float u_opaque;\n"
        "void main() {\n"
        "  vec4 c = texture(u_tex, v_uv);\n"
        "  o = (u_opaque > 0.5) ? vec4(c.rgb, 1.0) : c;\n"
        "}\n";
    unsigned vs = A.CreateShader(GL_VERTEX_SHADER_);
    A.ShaderSource(vs, 1, &VS, nullptr);
    A.CompileShader(vs);
    if (!shader_ok(vs, A, "vertex shader")) return g_ok = false;
    unsigned fs = A.CreateShader(GL_FRAGMENT_SHADER_);
    A.ShaderSource(fs, 1, &FS, nullptr);
    A.CompileShader(fs);
    if (!shader_ok(fs, A, "fragment shader")) return g_ok = false;
    unsigned prog = A.CreateProgram();
    A.AttachShader(prog, vs);
    A.AttachShader(prog, fs);
    A.LinkProgram(prog);
    int lok = 0;
    A.GetProgramiv(prog, GL_LINK_STATUS_, &lok);
    if (!lok) { printf("[gl] shader link failed\n"); return g_ok = false; }
    A.UseProgram(prog);
    g_prog       = prog;
    g_uni_tex    = 0;                        // the real location is fetched by name below
    g_uni_flip   = 1;
    g_uni_opaque = 2;
    // Fetch the real uniform locations with glGetUniformLocation.
    typedef int (*FnLoc)(unsigned, const char *);
    FnLoc loc = (FnLoc)sym(A, "glGetUniformLocation");
    if (loc) {
        g_uni_tex    = loc(prog, "u_tex");
        g_uni_flip   = loc(prog, "u_flip");
        g_uni_opaque = loc(prog, "u_opaque");
    }
    if (A.GenVertexArrays) {
        A.GenVertexArrays(1, &g_vao);
        A.BindVertexArray(g_vao);
    }

    printf(" %s|[gl] GPU composition ready: %s | %s| %s\n",
           (const char *)A.GetString(GL_VERSION_),
           (const char *)A.GetString(GL_RENDERER_));
    return g_ok = true;
}

inline bool ready() { return init(); }

// Rebuild the target FBO when the SurfaceFlinger buffer size changes. RGBA8 is sufficient.
static bool ensure_fbo(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (g_fbo && g_fw == w && g_fh == h) return true;
    if (g_fbo) { A.DeleteFramebuffers(1, &g_fbo); g_fbo = 0; }
    if (g_ftex) { A.DeleteTextures(1, &g_ftex); g_ftex = 0; }
    A.GenTextures(1, &g_ftex);
    A.BindTexture(GL_TEXTURE_2D_, g_ftex);
    A.TexImage2D(GL_TEXTURE_2D_, 0, GL_RGBA8_, w, h, 0, GL_RGBA_,
                 GL_UNSIGNED_BYTE_, nullptr);
    A.TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MIN_FILTER_, GL_LINEAR_);
    A.TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MAG_FILTER_, GL_LINEAR_);
    A.GenFramebuffers(1, &g_fbo);
    A.BindFramebuffer(GL_FRAMEBUFFER_, g_fbo);
    A.FramebufferTexture2D(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_,
                           GL_TEXTURE_2D_, g_ftex, 0);
    const unsigned st = A.CheckFramebufferStatus(GL_FRAMEBUFFER_);
    if (st != GL_FRAMEBUFFER_COMPLETE_) {
        printf("[gl] FBO incomplete 0x%x (%dx%d)\n", st, w, h);
        A.DeleteFramebuffers(1, &g_fbo); g_fbo = 0;
        A.DeleteTextures(1, &g_ftex);    g_ftex = 0;
        return false;
    }
    g_fw = w; g_fh = h;
    g_fbo_rebuild++;
    return true;
}

// Client dmabuf to texture, with a cache.
static unsigned tex_for(int fd, uint32_t w, uint32_t h, uint32_t fourcc,
                        uint32_t stride, uint32_t offset, uint64_t mod) {
    struct stat st;
    ino_t ino = 0;
    if (fstat(fd, &st) == 0) ino = st.st_ino;
    for (int i = 0; i < CACHE_N; i++) {
        CacheEnt &c = g_cache[i];
        if (c.fd == fd && c.ino == ino && c.w == w && c.h == h &&
            c.fourcc == fourcc && c.stride == stride && c.offset == offset && c.mod == mod)
            return c.tex;
    }
    // Take a free slot, otherwise reuse the oldest, which is slot 0.
    int slot = -1;
    for (int i = 0; i < CACHE_N; i++)
        if (g_cache[i].fd < 0) { slot = i; break; }
    if (slot < 0) {
        slot = 0;
        // Prefer a slot that does not hold the current fd
        for (int i = 0; i < CACHE_N; i++)
            if (g_cache[i].fd != fd) { slot = i; break; }
    }
    CacheEnt &c = g_cache[slot];
    if (c.tex) { A.DeleteTextures(1, &c.tex); c.tex = 0; }
    if (c.img && A.DestroyImage) { A.DestroyImage(g_dpy, c.img); c.img = nullptr; }
    c = CacheEnt();
    c.fd = fd; c.ino = ino; c.w = w; c.h = h;
    c.fourcc = fourcc; c.stride = stride; c.offset = offset; c.mod = mod;

    const EGLint ia[] = {
        EGL_WIDTH_,  (EGLint)w,
        EGL_HEIGHT_, (EGLint)h,
        EGL_LINUX_DRM_FOURCC_EXT_, (EGLint)fourcc,
        EGL_DMA_BUF_PLANE0_FD_EXT_, (EGLint)fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT_, (EGLint)offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT_, (EGLint)stride,
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT_, (EGLint)(mod & 0xffffffffu),
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT_, (EGLint)(mod >> 32),
        EGL_IMAGE_PRESERVED_KHR_, EGL_TRUE_,
        EGL_NONE_
    };
    c.img = A.CreateImage(g_dpy, EGL_NO_CONTEXT_, EGL_LINUX_DMA_BUF_EXT_, nullptr,
                          (const intptr_t *)ia);
    if (!c.img) {
        static int n = 0;
        if (n++ < 6)
            printf("[gl] eglCreateImageKHR failed fd=%d %ux%u fourcc=0x%08x stride=%u off=%u mod=0x%llx\n",
                   fd, w, h, fourcc, stride, offset, (unsigned long long)mod);
        c.fd = -1;
        return 0;
    }
    A.GenTextures(1, &c.tex);
    A.BindTexture(GL_TEXTURE_2D_, c.tex);
    A.TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MIN_FILTER_, GL_LINEAR_);
    A.TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MAG_FILTER_, GL_LINEAR_);
    A.TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_WRAP_S_, GL_CLAMP_TO_EDGE_);
    A.TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_WRAP_T_, GL_CLAMP_TO_EDGE_);
    A.EGLImageTargetTexture2DOES(GL_TEXTURE_2D_, c.img);
    const unsigned e = A.GetError();
    if (e) {
        static int n = 0;
        if (n++ < 6) printf("[gl] glEGLImageTargetTexture2DOES err=0x%x\n", e);
        A.DeleteTextures(1, &c.tex); c.tex = 0;
        if (A.DestroyImage) A.DestroyImage(g_dpy, c.img);
        c.img = nullptr; c.fd = -1;
        return 0;
    }
    return c.tex;
}

// Composite one client dmabuf into dst, the buffer returned by ANativeWindow_lock.
//   dst_w, dst_h     : pixel size of the SF buffer; all of it is written
//   dst_stride_px    : row stride of the SF buffer, in pixels
//   opaque           : write alpha 1, required for terminals and browsers
inline bool draw(int fd, uint32_t w, uint32_t h, uint32_t fourcc,
                 uint32_t stride, uint32_t offset, uint64_t mod,
                 void *dst, int dst_w, int dst_h, int dst_stride_px, bool opaque) {
    if (!ready() || !dst || dst_w <= 0 || dst_h <= 0) return false;
    if (g_flip < 0.f) g_flip = getenv("SFWM_GL_FLIP") ? 1.f : 0.f;

    const unsigned tex = tex_for(fd, w, h, fourcc, stride, offset, mod);
    if (!tex) return false;
    if (!ensure_fbo(dst_w, dst_h)) return false;

    A.BindFramebuffer(GL_FRAMEBUFFER_, g_fbo);
    A.Viewport(0, 0, dst_w, dst_h);
    // Clear to black first: leftover alpha 0 in a fresh SF buffer shows the desktop.
    A.ClearColor(0.f, 0.f, 0.f, 1.f);
    A.Clear(GL_COLOR_BUFFER_BIT_);

    A.UseProgram(g_prog);
    if (g_uni_tex >= 0)    A.Uniform1i(g_uni_tex, 0);
    if (g_uni_flip >= 0)   A.Uniform1f(g_uni_flip, g_flip);
    if (g_uni_opaque >= 0) A.Uniform1f(g_uni_opaque, opaque ? 1.f : 0.f);
    A.BindTexture(GL_TEXTURE_2D_, tex);
    A.DrawArrays(GL_TRIANGLE_STRIP_, 0, 4);
    const unsigned de = A.GetError();
    if (de) {
        static int n = 0;
        if (n++ < 6) printf("[gl] draw err=0x%x\n", de);
    }

    // Read straight into the SF buffer, with GL_PACK_ROW_LENGTH carrying the stride
    A.PixelStorei(GL_PACK_ALIGNMENT_, 4);
    A.PixelStorei(GL_PACK_ROW_LENGTH_, dst_stride_px);
    A.ReadPixels(0, 0, dst_w, dst_h, GL_BGRA_, GL_UNSIGNED_BYTE_, dst);
    A.PixelStorei(GL_PACK_ROW_LENGTH_, 0);
    const unsigned re = A.GetError();
    if (re) {
        static int n = 0;
        if (n++ < 6) printf("[gl] readPixels err=0x%x (%dx%d stride=%d)\n",
                           re, dst_w, dst_h, dst_stride_px);
        return false;
    }
    if (getenv("SFWM_GLDBG")) {
        static long n = 0;
        if (n++ < 100000)
            printf("[gldbg] db=%ux%u fourcc=0x%08x | dst=%dx%d stride=%d | fbo-rebuild=%d\n",
                   w, h, fourcc, dst_w, dst_h, dst_stride_px, g_fbo_rebuild);
    }
    return true;
}

}  // namespace glc
