// Copyright 2025 SW7FT. All rights reserved.

#include "ui/ozone/platform/qnx_screen/client_native_pixmap_factory_qnx_screen.h"

#include "ui/ozone/common/stub_client_native_pixmap_factory.h"

namespace ui {

gfx::ClientNativePixmapFactory* CreateClientNativePixmapFactoryQnx_screen() {
  return CreateStubClientNativePixmapFactory();
}

}  // namespace ui
