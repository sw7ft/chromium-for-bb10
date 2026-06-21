// Copyright 2025 SW7FT. All rights reserved.
// EGL/GLES2 GLOzone backend for the QNX Screen Ozone platform (Adreno 330).

#ifndef UI_OZONE_PLATFORM_QNX_SCREEN_QNX_SCREEN_GL_OZONE_EGL_H_
#define UI_OZONE_PLATFORM_QNX_SCREEN_QNX_SCREEN_GL_OZONE_EGL_H_

#include "base/memory/raw_ptr.h"
#include "ui/ozone/common/gl_ozone_egl.h"

namespace ui {

class QnxScreenWindowManager;

// Bridges Chromium's GL stack to BB10's native EGL/GLES2 (libEGL/libGLESv2)
// over a QNX Screen window. View surfaces present via eglSwapBuffers directly to
// the on-screen screen_window_t; offscreen surfaces use pbuffers.
class QnxScreenGLOzoneEGL : public GLOzoneEGL {
 public:
  explicit QnxScreenGLOzoneEGL(QnxScreenWindowManager* window_manager);
  ~QnxScreenGLOzoneEGL() override;

  QnxScreenGLOzoneEGL(const QnxScreenGLOzoneEGL&) = delete;
  QnxScreenGLOzoneEGL& operator=(const QnxScreenGLOzoneEGL&) = delete;

  // GLOzone:
  scoped_refptr<gl::GLContext> CreateGLContext(
      gl::GLShareGroup* share_group,
      gl::GLSurface* compatible_surface,
      const gl::GLContextAttribs& attribs) override;
  scoped_refptr<gl::GLSurface> CreateViewGLSurface(
      gl::GLDisplay* display,
      gfx::AcceleratedWidget window) override;
  scoped_refptr<gl::GLSurface> CreateOffscreenGLSurface(
      gl::GLDisplay* display,
      const gfx::Size& size) override;

 protected:
  // GLOzoneEGL:
  gl::EGLDisplayPlatform GetNativeDisplay() override;
  bool LoadGLES2Bindings(
      const gl::GLImplementationParts& implementation) override;

 private:
  raw_ptr<QnxScreenWindowManager> window_manager_;
};

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_QNX_SCREEN_QNX_SCREEN_GL_OZONE_EGL_H_
