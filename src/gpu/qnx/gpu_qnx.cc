// Copyright 2025 SW7FT. All rights reserved.
// GPU initialization for QNX ARM (Adreno 330)
//
// All EGL/GLES2 functions are loaded dynamically via dlsym().
// This avoids a hard link dependency on libEGL/libGLESv2 and
// allows graceful software fallback when GPU libs aren't present.

#ifdef __QNXNTO__

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

// === GL function pointer types ===
typedef const GLubyte* (*PFN_glGetString)(GLenum name);
typedef void (*PFN_glGetIntegerv)(GLenum pname, GLint* params);

namespace gpu {
namespace qnx {

// Forward declarations
void ShutdownGpu();

// Dynamic library handles
static void* g_egl_lib = NULL;
static void* g_gles2_lib = NULL;

// EGL function pointers
static PFN_eglGetDisplay     pfn_eglGetDisplay = NULL;
static PFN_eglInitialize     pfn_eglInitialize = NULL;
static PFN_eglChooseConfig   pfn_eglChooseConfig = NULL;
static PFN_eglTerminate      pfn_eglTerminate = NULL;

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

bool InitializeGpuLibraries() {
    g_egl_lib = dlopen("libEGL.so", RTLD_LAZY);
    if (!g_egl_lib) {
        fprintf(stderr, "GPU: Failed to load libEGL.so: %s\n", dlerror());
        return false;
    }

    g_gles2_lib = dlopen("libGLESv2.so", RTLD_LAZY);
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
        fprintf(stderr, "GPU: EGL not loaded\n");
        return false;
    }

    EGLDisplay display = pfn_eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
        fprintf(stderr, "GPU: eglGetDisplay failed\n");
        return false;
    }

    EGLint major, minor;
    if (!pfn_eglInitialize(display, &major, &minor)) {
        fprintf(stderr, "GPU: eglInitialize failed\n");
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

#endif  // __QNXNTO__
