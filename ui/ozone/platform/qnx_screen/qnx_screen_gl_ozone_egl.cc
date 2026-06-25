// Copyright 2025 SW7FT. All rights reserved.

#include "ui/ozone/platform/qnx_screen/qnx_screen_gl_ozone_egl.h"

#include <dlfcn.h>

#include "base/logging.h"
#include "base/memory/scoped_refptr.h"
#include "base/native_library.h"
#include "base/qnx_trace.h"
#include "ui/gl/gl_context.h"
#include "ui/gl/gl_context_egl.h"
#include "ui/gl/gl_display.h"
#include "ui/gl/gl_implementation.h"
#include "ui/gl/gl_surface.h"
#include "ui/gl/gl_surface_egl.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_window.h"
#include "ui/ozone/platform/qnx_screen/qnx_screen_window_manager.h"

namespace ui {
namespace {

// BB10 EGL requires a live connection to the Screen composition service (a
// screen_context_t) in this process before eglGetDisplay() will succeed. We
// dlopen libscreen and create one lazily; loaded with RTLD_GLOBAL so libGLESv2
// can also resolve any Screen-side symbols it needs.
typedef void* ScreenContext;
typedef int (*PFN_screen_create_context)(ScreenContext* pctx, int flags);
void* g_screen_lib = nullptr;
ScreenContext g_screen_ctx = nullptr;

// BB10's EGL advertises EGL_NV_post_sub_buffer, so Chromium's display
// compositor (SkiaOutputDeviceGL) presents only the damaged sub-rect via
// eglPostSubBufferNV after the first full frame. On this driver that partial
// present corrupts the window: the page renders correctly once, then tiles /
// shears across the panel on subsequent frames. Forcing SupportsPostSubBuffer()
// to false makes the compositor always swap full frames (the upstream
// disable_post_sub_buffers_for_onscreen_surfaces workaround does the same, but
// that path needs GPU-info-based bug-list matching which we don't have here).
class QnxNativeViewGLSurfaceEGL : public gl::NativeViewGLSurfaceEGL {
 public:
  QnxNativeViewGLSurfaceEGL(gl::GLDisplayEGL* display,
                            EGLNativeWindowType window)
      : gl::NativeViewGLSurfaceEGL(display, window, nullptr) {}

  bool SupportsPostSubBuffer() override { return false; }

 protected:
  ~QnxNativeViewGLSurfaceEGL() override = default;
};

void EnsureScreenContext() {
  if (g_screen_ctx)
    return;
  if (!g_screen_lib)
    g_screen_lib = dlopen("libscreen.so", RTLD_NOW | RTLD_GLOBAL);
  if (!g_screen_lib) {
    LOG(ERROR) << "QNX GL: dlopen libscreen.so failed: " << dlerror();
    return;
  }
  auto create_ctx = reinterpret_cast<PFN_screen_create_context>(
      dlsym(g_screen_lib, "screen_create_context"));
  if (!create_ctx) {
    LOG(ERROR) << "QNX GL: screen_create_context symbol missing";
    return;
  }
  if (create_ctx(&g_screen_ctx, 0) != 0)  // 0 == SCREEN_APPLICATION_CONTEXT
    LOG(ERROR) << "QNX GL: screen_create_context failed";
  else
    fprintf(stderr, "QNX GL: screen context created on GL thread\n");
}

}  // namespace

QnxScreenGLOzoneEGL::QnxScreenGLOzoneEGL(QnxScreenWindowManager* window_manager)
    : window_manager_(window_manager) {}

QnxScreenGLOzoneEGL::~QnxScreenGLOzoneEGL() = default;

scoped_refptr<gl::GLContext> QnxScreenGLOzoneEGL::CreateGLContext(
    gl::GLShareGroup* share_group,
    gl::GLSurface* compatible_surface,
    const gl::GLContextAttribs& attribs) {
  // The BB10 Adreno 330 driver advertises OpenGL ES 2.0 yet still exports ES3
  // entry points that dispatch through null (e.g. glGetInternalformativ -> pc=0).
  // Chromium defaults to requesting an ES3 context (client_major_es_version = 3)
  // and then probes ES3 features, which crashes. Force every context to ES2 so
  // the ES3 code paths are never taken.
  gl::GLContextAttribs es2 = attribs;
  es2.client_major_es_version = 2;
  es2.client_minor_es_version = 0;
  QNX_GL_LOG_MSG("QNX GL TRACE: CreateGLContext (forced ES2)\n");
  scoped_refptr<gl::GLContext> ctx = gl::InitializeGLContext(
      base::MakeRefCounted<gl::GLContextEGL>(share_group), compatible_surface,
      es2);
  QNX_GL_LOG("QNX GL TRACE: CreateGLContext -> %s\n", ctx ? "ok" : "NULL");
  return ctx;
}

scoped_refptr<gl::GLSurface> QnxScreenGLOzoneEGL::CreateViewGLSurface(
    gl::GLDisplay* display,
    gfx::AcceleratedWidget widget) {
  QnxScreenWindow* window = window_manager_->GetWindow(widget);
  if (!window || !window->screen_window()) {
    LOG(ERROR) << "QNX GL: no screen_window for widget " << widget;
    return nullptr;
  }
  // On QNX, EGLNativeWindowType is khronos_uintptr_t; the BB10 driver expects
  // the screen_window_t handle passed straight to eglCreateWindowSurface.
  EGLNativeWindowType native =
      reinterpret_cast<EGLNativeWindowType>(window->screen_window());
  QNX_GL_LOG("QNX GL TRACE: CreateViewGLSurface widget=%u win=%p\n", widget,
             window->screen_window());
  scoped_refptr<gl::GLSurface> surface = gl::InitializeGLSurface(
      base::MakeRefCounted<QnxNativeViewGLSurfaceEGL>(
          display->GetAs<gl::GLDisplayEGL>(), native));
  if (surface) {
    gfx::Size sz = surface->GetSize();
    QNX_GL_LOG(
        "QNX GL TRACE: CreateViewGLSurface -> ok, EGL surface size=%dx%d\n",
        sz.width(), sz.height());
  } else {
    QNX_GL_LOG_MSG(
        "QNX GL TRACE: CreateViewGLSurface -> NULL "
        "(eglCreateWindowSurface failed)\n");
  }
  return surface;
}

scoped_refptr<gl::GLSurface> QnxScreenGLOzoneEGL::CreateOffscreenGLSurface(
    gl::GLDisplay* display,
    const gfx::Size& size) {
  QNX_GL_LOG("QNX GL TRACE: CreateOffscreenGLSurface %dx%d\n", size.width(),
             size.height());
  return gl::InitializeGLSurface(base::MakeRefCounted<gl::PbufferGLSurfaceEGL>(
      display->GetAs<gl::GLDisplayEGL>(), size));
}

gl::EGLDisplayPlatform QnxScreenGLOzoneEGL::GetNativeDisplay() {
  EnsureScreenContext();
  fprintf(stderr, "QNX GL: GetNativeDisplay using EGL_DEFAULT_DISPLAY "
                    "(screen_context=%p)\n",
          g_screen_ctx);
  // NOTE: EGL only initializes when the app is launched by TAPPING the icon
  // (a real Navigator graphics session). Over SSH, eglGetDisplay returns
  // EGL_NO_DISPLAY / EGL_NOT_INITIALIZED (0x3001) and the GPU process exits.
  // This is expected, not a bug. See deploy/GPU_EGL_NOTES.md.
  //
  // Diagnostic: the gl stack logs only "Initialization of all EGL display types
  // failed" without the EGL error code. Probe eglGetDisplay/eglInitialize
  // directly here and surface eglGetError so we know the real cause.
  {
    void* egl = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
    if (egl) {
      using PFN_gd = void* (*)(void*);
      using PFN_init = unsigned int (*)(void*, int*, int*);
      using PFN_err = int (*)(void);
      using PFN_bind = unsigned int (*)(unsigned int);
      auto egl_get_display = reinterpret_cast<PFN_gd>(dlsym(egl, "eglGetDisplay"));
      auto egl_initialize = reinterpret_cast<PFN_init>(dlsym(egl, "eglInitialize"));
      auto egl_get_error = reinterpret_cast<PFN_err>(dlsym(egl, "eglGetError"));
      auto egl_bind_api = reinterpret_cast<PFN_bind>(dlsym(egl, "eglBindAPI"));
      if (egl_get_display && egl_initialize && egl_get_error) {
        unsigned int bound =
            egl_bind_api ? egl_bind_api(0x30A0u /*EGL_OPENGL_ES_API*/) : 2u;
        void* dpy = egl_get_display(nullptr /*EGL_DEFAULT_DISPLAY*/);
        int err_get = egl_get_error();
        int major = 0, minor = 0;
        unsigned int ok = dpy ? egl_initialize(dpy, &major, &minor) : 0u;
        int err_init = egl_get_error();
        fprintf(stderr,
                "QNX GL PROBE: bindAPI=%u getDisplay=%p (err=0x%x) "
                "initialize=%u v%d.%d (err=0x%x)\n",
                bound, dpy, err_get, ok, major, minor, err_init);
      } else {
        fprintf(stderr,
                "QNX GL PROBE: missing EGL symbols gd=%p init=%p err=%p\n",
                reinterpret_cast<void*>(egl_get_display),
                reinterpret_cast<void*>(egl_initialize),
                reinterpret_cast<void*>(egl_get_error));
      }
    } else {
      fprintf(stderr, "QNX GL PROBE: dlopen libEGL.so failed: %s\n", dlerror());
    }
  }
  return gl::EGLDisplayPlatform(EGL_DEFAULT_DISPLAY);
}

bool QnxScreenGLOzoneEGL::LoadGLES2Bindings(
    const gl::GLImplementationParts& implementation) {
  EnsureScreenContext();

  // Load libEGL FIRST and with RTLD_GLOBAL: on BB10 libGLESv2 resolves internal
  // symbols (_egl_tls, _egl_default_context, _glesGetProcAddressProc, ...) from
  // libEGL's global scope. The stock loader (egl_util.cc) loads GLES first,
  // without RTLD_GLOBAL, and uses libGLESv2.so.2 (device has .so.1) - all wrong
  // for QNX, hence this custom path.
  void* egl = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
  if (!egl) {
    LOG(ERROR) << "QNX GL: dlopen libEGL.so failed: " << dlerror();
    return false;
  }
  void* gles = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
  if (!gles) {
    LOG(ERROR) << "QNX GL: dlopen libGLESv2.so failed: " << dlerror();
    return false;
  }

  auto get_proc = reinterpret_cast<gl::GLGetProcAddressProc>(
      dlsym(egl, "eglGetProcAddress"));
  if (!get_proc) {
    LOG(ERROR) << "QNX GL: eglGetProcAddress not found in libEGL";
    return false;
  }
  gl::SetGLGetProcAddressProc(get_proc);
  gl::AddGLNativeLibrary(static_cast<base::NativeLibrary>(egl));
  gl::AddGLNativeLibrary(static_cast<base::NativeLibrary>(gles));
  LOG(WARNING) << "QNX GL: native EGL/GLES2 bindings loaded";
  return true;
}

}  // namespace ui
