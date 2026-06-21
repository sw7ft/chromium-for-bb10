// Copyright 2025 SW7FT. All rights reserved.
// GPU initialization for QNX ARM (Adreno 330)
//
// All EGL/GLES2 functions are loaded dynamically via dlsym().
// This avoids a hard link dependency on libEGL/libGLESv2 and
// allows graceful software fallback when GPU libs aren't present.

#include "build/build_config.h"

#if BUILDFLAG(IS_QNX)

#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>

// We only need the EGL/GL type definitions and constants,
// not the function declarations. Define types manually to
// avoid pulling in the full headers (which create link deps).

// === EGL types and constants ===
typedef void* EGLDisplay;
typedef void* EGLConfig;
typedef void* EGLSurface;
typedef void* EGLContext;
typedef unsigned int EGLBoolean;
typedef int EGLint;

#define EGL_DEFAULT_DISPLAY ((void*)0)
#define EGL_NO_DISPLAY      ((EGLDisplay)0)
#define EGL_FALSE           0
#define EGL_TRUE            1

#define EGL_SURFACE_TYPE      0x3033
#define EGL_WINDOW_BIT        0x0004
#define EGL_RENDERABLE_TYPE   0x3040
#define EGL_OPENGL_ES2_BIT    0x0004
#define EGL_RED_SIZE          0x3024
#define EGL_GREEN_SIZE        0x3023
#define EGL_BLUE_SIZE         0x3022
#define EGL_ALPHA_SIZE        0x3021
#define EGL_DEPTH_SIZE        0x3025
#define EGL_STENCIL_SIZE      0x3026
#define EGL_NONE              0x3038
#define EGL_PBUFFER_BIT       0x0001
#define EGL_WIDTH             0x3057
#define EGL_HEIGHT            0x3056
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_OPENGL_ES_API     0x30A0
#define EGL_NO_CONTEXT        ((EGLContext)0)
#define EGL_NO_SURFACE        ((EGLSurface)0)

// === GL types and constants ===
typedef unsigned int GLenum;
typedef int GLint;
typedef unsigned char GLubyte;

#define GL_VENDOR          0x1F00
#define GL_RENDERER        0x1F01
#define GL_VERSION         0x1F02
#define GL_MAX_TEXTURE_SIZE 0x0D33

// === EGL function pointer types ===
typedef EGLDisplay (*PFN_eglGetDisplay)(void* native_display);
typedef EGLBoolean (*PFN_eglInitialize)(EGLDisplay dpy, EGLint* major, EGLint* minor);
typedef EGLBoolean (*PFN_eglChooseConfig)(EGLDisplay dpy, const EGLint* attribs,
                                          EGLConfig* configs, EGLint config_size,
                                          EGLint* num_config);
typedef EGLBoolean (*PFN_eglTerminate)(EGLDisplay dpy);
typedef EGLBoolean (*PFN_eglBindAPI)(unsigned int api);
typedef EGLSurface (*PFN_eglCreatePbufferSurface)(EGLDisplay dpy, EGLConfig config,
                                                  const EGLint* attribs);
typedef EGLContext (*PFN_eglCreateContext)(EGLDisplay dpy, EGLConfig config,
                                           EGLContext share, const EGLint* attribs);
typedef EGLBoolean (*PFN_eglMakeCurrent)(EGLDisplay dpy, EGLSurface draw,
                                         EGLSurface read, EGLContext ctx);
typedef EGLint (*PFN_eglGetError)(void);

// === GL function pointer types ===
typedef const GLubyte* (*PFN_glGetString)(GLenum name);
typedef void (*PFN_glGetIntegerv)(GLenum pname, GLint* params);

namespace gpu {
namespace qnx {

// Forward declarations
void ShutdownGpu();
void PrintGpuInfo();

// Dynamic library handles
static void* g_egl_lib = NULL;
static void* g_gles2_lib = NULL;
static void* g_screen_lib = NULL;

// QNX Screen: EGL on BB10 needs a live connection to the Screen composition
// service (a screen_context_t) before eglGetDisplay() will succeed.
typedef void* ScreenContext;
typedef int (*PFN_screen_create_context)(ScreenContext* pctx, int flags);
static ScreenContext g_screen_ctx = NULL;

// EGL function pointers
static PFN_eglGetDisplay     pfn_eglGetDisplay = NULL;
static PFN_eglInitialize     pfn_eglInitialize = NULL;
static PFN_eglChooseConfig   pfn_eglChooseConfig = NULL;
static PFN_eglTerminate      pfn_eglTerminate = NULL;
static PFN_eglBindAPI        pfn_eglBindAPI = NULL;
static PFN_eglCreatePbufferSurface pfn_eglCreatePbufferSurface = NULL;
static PFN_eglCreateContext  pfn_eglCreateContext = NULL;
static PFN_eglMakeCurrent    pfn_eglMakeCurrent = NULL;
static PFN_eglGetError       pfn_eglGetError = NULL;

// GL function pointers
static PFN_glGetString       pfn_glGetString = NULL;
static PFN_glGetIntegerv     pfn_glGetIntegerv = NULL;

static void* LoadSym(void* lib, const char* name) {
    void* sym = dlsym(lib, name);
    if (!sym) {
        fprintf(stderr, "GPU: Failed to load %s: %s\n", name, dlerror());
    }
    return sym;
}

static bool EnsureScreenContext() {
    if (g_screen_ctx)
        return true;
    g_screen_lib = dlopen("libscreen.so", RTLD_NOW | RTLD_GLOBAL);
    if (!g_screen_lib) {
        fprintf(stderr, "GPU: failed to load libscreen.so: %s\n", dlerror());
        return false;
    }
    PFN_screen_create_context create_ctx =
        (PFN_screen_create_context)dlsym(g_screen_lib, "screen_create_context");
    if (!create_ctx) {
        fprintf(stderr, "GPU: screen_create_context symbol missing\n");
        return false;
    }
    // 0 == SCREEN_APPLICATION_CONTEXT
    if (create_ctx(&g_screen_ctx, 0) != 0) {
        fprintf(stderr, "GPU: screen_create_context failed\n");
        return false;
    }
    fprintf(stderr, "GPU: screen context created\n");
    return true;
}

bool InitializeGpuLibraries() {
    // On BB10/QNX, libGLESv2 is tightly coupled to internal symbols that live
    // in libEGL (_egl_tls, _egl_default_context, _glesGetProcAddressProc, ...).
    // libEGL MUST be loaded with RTLD_GLOBAL so those symbols enter the global
    // scope; otherwise libGLESv2 fails to load with "unknown symbol" errors.
    g_egl_lib = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
    if (!g_egl_lib) {
        fprintf(stderr, "GPU: Failed to load libEGL.so: %s\n", dlerror());
        return false;
    }

    g_gles2_lib = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    if (!g_gles2_lib) {
        fprintf(stderr, "GPU: Failed to load libGLESv2.so: %s\n", dlerror());
        dlclose(g_egl_lib);
        g_egl_lib = NULL;
        return false;
    }

    // Resolve EGL entry points
    pfn_eglGetDisplay   = (PFN_eglGetDisplay)LoadSym(g_egl_lib, "eglGetDisplay");
    pfn_eglInitialize   = (PFN_eglInitialize)LoadSym(g_egl_lib, "eglInitialize");
    pfn_eglChooseConfig = (PFN_eglChooseConfig)LoadSym(g_egl_lib, "eglChooseConfig");
    pfn_eglTerminate    = (PFN_eglTerminate)LoadSym(g_egl_lib, "eglTerminate");
    pfn_eglBindAPI      = (PFN_eglBindAPI)LoadSym(g_egl_lib, "eglBindAPI");
    pfn_eglCreatePbufferSurface =
        (PFN_eglCreatePbufferSurface)LoadSym(g_egl_lib, "eglCreatePbufferSurface");
    pfn_eglCreateContext =
        (PFN_eglCreateContext)LoadSym(g_egl_lib, "eglCreateContext");
    pfn_eglMakeCurrent  = (PFN_eglMakeCurrent)LoadSym(g_egl_lib, "eglMakeCurrent");
    pfn_eglGetError     = (PFN_eglGetError)LoadSym(g_egl_lib, "eglGetError");

    // Resolve GL entry points
    pfn_glGetString     = (PFN_glGetString)LoadSym(g_gles2_lib, "glGetString");
    pfn_glGetIntegerv   = (PFN_glGetIntegerv)LoadSym(g_gles2_lib, "glGetIntegerv");

    if (!pfn_eglGetDisplay || !pfn_eglInitialize || !pfn_eglChooseConfig) {
        fprintf(stderr, "GPU: Missing critical EGL functions\n");
        ShutdownGpu();
        return false;
    }

    fprintf(stderr, "GPU: EGL and GLES2 libraries loaded (dynamic)\n");
    return true;
}

bool InitializeEglDisplay(void** out_display, void** out_config) {
    if (!pfn_eglGetDisplay || !pfn_eglInitialize || !pfn_eglChooseConfig) {
        if (!InitializeGpuLibraries())
            return false;
    }
    if (!EnsureScreenContext())
        return false;

    EGLDisplay display = pfn_eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
        fprintf(stderr, "GPU: eglGetDisplay failed (err=0x%x)\n",
                pfn_eglGetError ? pfn_eglGetError() : 0);
        return false;
    }

    EGLint major, minor;
    if (!pfn_eglInitialize(display, &major, &minor)) {
        fprintf(stderr, "GPU: eglInitialize failed (err=0x%x)\n",
                pfn_eglGetError ? pfn_eglGetError() : 0);
        return false;
    }

    fprintf(stderr, "GPU: EGL %d.%d initialized\n", major, minor);

    // Request GLES2 context
    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };

    EGLConfig config;
    EGLint num_configs;
    if (!pfn_eglChooseConfig(display, config_attribs, &config, 1, &num_configs) ||
        num_configs == 0) {
        fprintf(stderr, "GPU: eglChooseConfig failed\n");
        return false;
    }

    *out_display = display;
    *out_config = config;

    fprintf(stderr, "GPU: EGL config selected (GLES2, RGBA8888, D24S8)\n");
    return true;
}

bool WarmUpEglEarly() {
    void* display = NULL;
    void* config = NULL;
    if (!InitializeEglDisplay(&display, &config))
        return false;
    fprintf(stderr, "GPU: early EGL warm-up ok display=%p config=%p\n",
            display, config);
    return true;
}

// Full probe: bring up an EGL display, create a tiny pbuffer + GLES2 context,
// make it current, and print the real GL strings. This proves the Adreno
// driver is actually usable (glGetString returns null without a current
// context). Returns true if a context was made current.
bool ProbeGpuContext() {
    if (!pfn_eglGetDisplay || !pfn_eglInitialize || !pfn_eglChooseConfig ||
        !pfn_eglCreatePbufferSurface || !pfn_eglCreateContext ||
        !pfn_eglMakeCurrent) {
        fprintf(stderr, "GPU: probe missing EGL entry points\n");
        return false;
    }

    // BB10 EGL requires a Screen connection in this process first.
    if (!EnsureScreenContext())
        return false;

    EGLDisplay display = pfn_eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
        fprintf(stderr, "GPU: eglGetDisplay failed (err=0x%x)\n",
                pfn_eglGetError ? pfn_eglGetError() : 0);
        return false;
    }
    EGLint major = 0, minor = 0;
    if (!pfn_eglInitialize(display, &major, &minor)) {
        fprintf(stderr, "GPU: eglInitialize failed\n");
        return false;
    }
    fprintf(stderr, "GPU: EGL %d.%d initialized\n", major, minor);

    if (pfn_eglBindAPI)
        pfn_eglBindAPI(EGL_OPENGL_ES_API);

    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config = NULL;
    EGLint num_configs = 0;
    if (!pfn_eglChooseConfig(display, config_attribs, &config, 1, &num_configs) ||
        num_configs == 0) {
        fprintf(stderr, "GPU: eglChooseConfig(pbuffer) failed\n");
        return false;
    }

    EGLint pbuffer_attribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    EGLSurface surface =
        pfn_eglCreatePbufferSurface(display, config, pbuffer_attribs);
    if (surface == EGL_NO_SURFACE) {
        fprintf(stderr, "GPU: eglCreatePbufferSurface failed (err=0x%x)\n",
                pfn_eglGetError ? pfn_eglGetError() : 0);
        return false;
    }

    EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext context =
        pfn_eglCreateContext(display, config, EGL_NO_CONTEXT, ctx_attribs);
    if (context == EGL_NO_CONTEXT) {
        fprintf(stderr, "GPU: eglCreateContext failed (err=0x%x)\n",
                pfn_eglGetError ? pfn_eglGetError() : 0);
        return false;
    }

    if (!pfn_eglMakeCurrent(display, surface, surface, context)) {
        fprintf(stderr, "GPU: eglMakeCurrent failed (err=0x%x)\n",
                pfn_eglGetError ? pfn_eglGetError() : 0);
        return false;
    }

    fprintf(stderr, "GPU: GLES2 context current (pbuffer 1x1)\n");
    PrintGpuInfo();
    return true;
}

void PrintGpuInfo() {
    if (!pfn_glGetString) {
        fprintf(stderr, "GPU Info: GL not loaded (software mode)\n");
        return;
    }

    const GLubyte* vendor   = pfn_glGetString(GL_VENDOR);
    const GLubyte* renderer = pfn_glGetString(GL_RENDERER);
    const GLubyte* version  = pfn_glGetString(GL_VERSION);

    fprintf(stderr, "GPU Info:\n");
    fprintf(stderr, "  GL_VENDOR:   %s\n", vendor ? (const char*)vendor : "(null)");
    fprintf(stderr, "  GL_RENDERER: %s\n", renderer ? (const char*)renderer : "(null)");
    fprintf(stderr, "  GL_VERSION:  %s\n", version ? (const char*)version : "(null)");

    if (pfn_glGetIntegerv) {
        GLint max_tex = 0;
        pfn_glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
        fprintf(stderr, "  MAX_TEXTURE: %d\n", max_tex);
    }
}

void ShutdownGpu() {
    pfn_eglGetDisplay = NULL;
    pfn_eglInitialize = NULL;
    pfn_eglChooseConfig = NULL;
    pfn_eglTerminate = NULL;
    pfn_glGetString = NULL;
    pfn_glGetIntegerv = NULL;

    if (g_gles2_lib) { dlclose(g_gles2_lib); g_gles2_lib = NULL; }
    if (g_egl_lib)   { dlclose(g_egl_lib);   g_egl_lib = NULL; }

    fprintf(stderr, "GPU: Shutdown complete\n");
}

}  // namespace qnx
}  // namespace gpu

#endif  // BUILDFLAG(IS_QNX)
