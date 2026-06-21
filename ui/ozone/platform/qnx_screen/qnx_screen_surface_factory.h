// Copyright 2025 SW7FT. All rights reserved.
// Software surface factory for QNX Screen Ozone platform

#ifndef UI_OZONE_PLATFORM_QNX_SCREEN_QNX_SCREEN_SURFACE_FACTORY_H_
#define UI_OZONE_PLATFORM_QNX_SCREEN_QNX_SCREEN_SURFACE_FACTORY_H_

#include <memory>

#include "ui/ozone/public/surface_factory_ozone.h"

namespace ui {

class QnxScreenWindowManager;
class QnxScreenGLOzoneEGL;

class QnxScreenSurfaceFactory : public SurfaceFactoryOzone {
 public:
  explicit QnxScreenSurfaceFactory(QnxScreenWindowManager* window_manager);
  ~QnxScreenSurfaceFactory() override;

  // SurfaceFactoryOzone:
  std::vector<gl::GLImplementationParts> GetAllowedGLImplementations() override;
  GLOzone* GetGLOzone(const gl::GLImplementationParts& implementation) override;
  std::unique_ptr<SurfaceOzoneCanvas> CreateCanvasForWidget(
      gfx::AcceleratedWidget widget) override;

 private:
  QnxScreenWindowManager* window_manager_;
  std::unique_ptr<QnxScreenGLOzoneEGL> egl_ozone_;
};

}  // namespace ui

#endif
