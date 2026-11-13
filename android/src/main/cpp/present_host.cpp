// SPDX-License-Identifier: AGPL-3.0-or-later
// Unprivileged presentation host.
//
// Runs inside an Android app process (loaded by the LSPosed module), takes a SurfaceView's
// Surface and presents frames that sfwm sends it over a socket. This is the arrangement
// Termux-X11 uses: the process that owns the Surface is the one that calls
// eglCreateWindowSurface, so no privilege is needed and no foreign buffer ever has to be
// imported into SurfaceFlinger.
//
// Why it is fast: every frame lands in a buffer the app's own BufferQueue already owns, and
// eglSwapInterval(0) keeps the producer from blocking on vsync, so sfwm is never throttled
// by the panel.
//
// Wire format, one frame per message (little endian):
//   uint32 magic 'SFWO' | width | height | drm fourcc | stride bytes   (20 bytes)
//   plus one SCM_RIGHTS dma-buf fd
// The host answers with int32 status, then keeps the connection.
#define LOG_TAG "sfwm-present"
#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

#define SFWM_OUT_MAGIC 0x5346574Fu   // 'SFWO'
#define SFWM_OUT_ABSTRACT "sfwm-out"

namespace {

struct Gl {
    PFNEGLCREATEIMAGEKHRPROC CreateImage = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC DestroyImage = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC ImageTargetTexture = nullptr;
    bool ok = false;
};

struct Host {
    ANativeWindow *win = nullptr;
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLSurface sfc = EGL_NO_SURFACE;
    EGLConfig cfg = nullptr;
    Gl gl;
    GLuint tex = 0;
    GLuint prog = 0;
    int w = 0, h = 0;
    int srv = -1;
    bool running = false;
};

Host g_host;
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

const char *kVert =
    "attribute vec2 a_pos;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "  v_uv = vec2(a_pos.x * 0.5 + 0.5, 0.5 - a_pos.y * 0.5);\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

const char *kFrag =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "void main() { gl_FragColor = texture2D(u_tex, v_uv); }\n";

GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glGetShaderInfoLog(s, sizeof log - 1, nullptr, log);
        LOGI("shader failed: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

bool make_program(Host *h) {
    GLuint vs = compile(GL_VERTEX_SHADER, kVert);
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFrag);
    if (!vs || !fs) return false;
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "a_pos");
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        LOGI("link failed");
        glDeleteProgram(p);
        return false;
    }
    h->prog = p;
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "u_tex"), 0);
    return true;
}

bool ensure_egl(Host *h) {
    if (h->ctx != EGL_NO_CONTEXT) return true;
    if (!h->win) return false;

    h->dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (h->dpy == EGL_NO_DISPLAY) return false;
    if (!eglInitialize(h->dpy, nullptr, nullptr)) {
        LOGI("eglInitialize failed: 0x%x", eglGetError());
        return false;
    }
    const EGLint cfgattr[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE,
                              EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                              EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLint n = 0;
    if (!eglChooseConfig(h->dpy, cfgattr, &h->cfg, 1, &n) || n < 1) {
        LOGI("no EGL config");
        return false;
    }
    h->sfc = eglCreateWindowSurface(h->dpy, h->cfg, h->win, nullptr);
    if (h->sfc == EGL_NO_SURFACE) {
        LOGI("eglCreateWindowSurface failed: 0x%x", eglGetError());
        return false;
    }
    const EGLint ctxattr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    h->ctx = eglCreateContext(h->dpy, h->cfg, EGL_NO_CONTEXT, ctxattr);
    if (h->ctx == EGL_NO_CONTEXT || !eglMakeCurrent(h->dpy, h->sfc, h->sfc, h->ctx)) {
        LOGI("context setup failed: 0x%x", eglGetError());
        return false;
    }
    // Async mode: the producer never waits for vsync, which is what lets sfwm run ahead.
    eglSwapInterval(h->dpy, 0);

    h->gl.CreateImage = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    h->gl.DestroyImage = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    h->gl.ImageTargetTexture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
        "glEGLImageTargetTexture2DOES");
    if (!h->gl.CreateImage || !h->gl.DestroyImage || !h->gl.ImageTargetTexture) {
        LOGI("missing EGL image entry points");
        return false;
    }
    if (!h->tex) {
        glGenTextures(1, &h->tex);
        glBindTexture(GL_TEXTURE_2D, h->tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    if (!h->prog) {
        if (!make_program(h)) return false;
    }
    const EGLint w = 1, hgt = 1;
    (void)w;
    (void)hgt;
    h->gl.ok = true;
    LOGI("EGL ready: %s | %s", glGetString(GL_VENDOR), glGetString(GL_RENDERER));
    return true;
}

void draw_frame(Host *h, const int fds[4], const uint32_t native_fmt[4], int nplanes,
                uint32_t w, uint32_t hgt, uint32_t stride) {
    EGLint attrs[16];
    int a = 0;
    attrs[a++] = EGL_WIDTH;
    attrs[a++] = (EGLint)w;
    attrs[a++] = EGL_HEIGHT;
    attrs[a++] = (EGLint)hgt;
    attrs[a++] = EGL_LINUX_DRM_FOURCC_EXT;
    attrs[a++] = (EGLint)native_fmt[0];
    attrs[a++] = EGL_DMA_BUF_PLANE0_FD_EXT;
    attrs[a++] = (EGLint)fds[0];
    attrs[a++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
    attrs[a++] = 0;
    attrs[a++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
    attrs[a++] = (EGLint)stride;
    attrs[a++] = EGL_NONE;
    if (nplanes > 1) {
        // Not expected for the formats sfwm sends; keep the first plane only.
        (void)0;
    }

    EGLImageKHR img = h->gl.CreateImage(h->dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr,
                                        attrs);
    if (img == EGL_NO_IMAGE_KHR) return;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, h->tex);
    h->gl.ImageTargetTexture(GL_TEXTURE_2D, (GLeglImageOES)img);
    glViewport(0, 0, h->w, h->h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    static const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    eglSwapBuffers(h->dpy, h->sfc);
    h->gl.DestroyImage(h->dpy, img);
}

int recv_frame(int sock, uint32_t hdr[5], int *out_fd) {
    struct iovec iov = {hdr, sizeof(uint32_t) * 5};
    char ctl[CMSG_SPACE(sizeof(int))];
    memset(ctl, 0, sizeof ctl);
    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctl;
    msg.msg_controllen = sizeof ctl;
    if (recvmsg(sock, &msg, 0) != (ssize_t)(sizeof(uint32_t) * 5)) return -1;
    *out_fd = -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            *out_fd = *(int *)CMSG_DATA(c);
            break;
        }
    return *out_fd >= 0 ? 0 : -1;
}

void *serve(void *) {
    for (;;) {
        const int c = accept4(g_host.srv, nullptr, nullptr, SOCK_CLOEXEC);
        if (c < 0) {
            if (errno == EINTR) continue;
            usleep(50000);
            continue;
        }
        LOGI("sfwm connected");
        for (;;) {
            uint32_t hdr[5];
            int fd = -1;
            if (recv_frame(c, hdr, &fd) != 0) break;
            if (hdr[0] != SFWM_OUT_MAGIC) {
                close(fd);
                const int32_t bad = -EINVAL;
                send(c, &bad, sizeof bad, MSG_NOSIGNAL);
                continue;
            }
            const uint32_t w = hdr[1], hgt = hdr[2], fourcc = hdr[3], stride = hdr[4];
            pthread_mutex_lock(&g_lock);
            int32_t status = 0;
            if (!g_host.win || !ensure_egl(&g_host)) {
                status = -ENODEV;
            } else {
                if (g_host.w != (int)w || g_host.h != (int)hgt) {
                    g_host.w = (int)w;
                    g_host.h = (int)hgt;
                }
                const uint32_t fmt[4] = {fourcc, 0, 0, 0};
                const int fds[4] = {fd, -1, -1, -1};
                draw_frame(&g_host, fds, fmt, 1, w, hgt, stride);
            }
            pthread_mutex_unlock(&g_lock);
            send(c, &status, sizeof status, MSG_NOSIGNAL);
            close(fd);
        }
        close(c);
        LOGI("sfwm disconnected");
    }
    return nullptr;
}

bool start_server() {
    if (g_host.srv >= 0) return true;
    const int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) return false;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    const char *name = SFWM_OUT_ABSTRACT;
    memcpy(addr.sun_path + 1, name, strlen(name));
    const socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name));
    unlink("");
    if (bind(s, (struct sockaddr *)&addr, len) != 0) {
        LOGI("bind(@%s) failed: %d", name, errno);
        close(s);
        return false;
    }
    if (listen(s, 2) != 0) {
        close(s);
        return false;
    }
    g_host.srv = s;
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 512 * 1024);
    const int rc = pthread_create(&t, &a, serve, nullptr);
    pthread_attr_destroy(&a);
    LOGI("listening on @%s (pthread_create=%d)", name, rc);
    return rc == 0;
}

}   // namespace

extern "C" JNIEXPORT jboolean JNICALL
Java_org_sfwm_bridge_display_PresentHost_nativeStart(JNIEnv *env, jclass, jobject jsurface,
                                                     jint width, jint height) {
    pthread_mutex_lock(&g_lock);
    if (g_host.win) {
        ANativeWindow_release(g_host.win);
        g_host.win = nullptr;
    }
    if (g_host.sfc != EGL_NO_SURFACE) {
        eglMakeCurrent(g_host.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(g_host.dpy, g_host.sfc);
        g_host.sfc = EGL_NO_SURFACE;
    }
    g_host.win = jsurface ? ANativeWindow_fromSurface(env, jsurface) : nullptr;
    g_host.w = width;
    g_host.h = height;
    const bool ready = g_host.win != nullptr && ensure_egl(&g_host);
    pthread_mutex_unlock(&g_lock);
    if (!ready) return JNI_FALSE;
    return start_server() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_sfwm_bridge_display_PresentHost_nativeStop(JNIEnv *, jclass) {
    pthread_mutex_lock(&g_lock);
    if (g_host.sfc != EGL_NO_SURFACE) {
        eglMakeCurrent(g_host.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(g_host.dpy, g_host.sfc);
        g_host.sfc = EGL_NO_SURFACE;
    }
    if (g_host.ctx != EGL_NO_CONTEXT) {
        eglDestroyContext(g_host.dpy, g_host.ctx);
        g_host.ctx = EGL_NO_CONTEXT;
    }
    if (g_host.win) {
        ANativeWindow_release(g_host.win);
        g_host.win = nullptr;
    }
    pthread_mutex_unlock(&g_lock);
    LOGI("stopped");
}
