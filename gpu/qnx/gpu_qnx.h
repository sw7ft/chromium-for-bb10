// Copyright 2025 SW7FT. All rights reserved.
// GPU compositing configuration for QNX ARM (BlackBerry Passport)
//
// The Passport has an Adreno 330 GPU supporting:
//   - OpenGL ES 3.0 (GLES3)
//   - OpenGL ES 2.0 (GLES2)
//   - EGL 1.4
//   - OpenCL 1.1
//
// QNX provides GPU access through:
//   - libEGL.so    (EGL 1.4 implementation)
//   - libGLESv2.so (GLES 2.0/3.0 implementation)
//   - libscreen.so (QNX native compositor / window manager)
//
// Chromium's rendering pipeline on QNX:
//
//   Option A: Headless (initial)
//     content_shell --headless --disable-gpu
//     Software rendering, no display output
//     Useful for testing page loading/parsing
//
//   Option B: X11 via Ozone (intermediate)
//     content_shell --ozone-platform=x11
//     Uses X11 Ozone backend (X server on device via Android layer)
//     EGL/GLES2 for GPU acceleration
//
//   Option C: Native QNX Screen (final)
//     content_shell --ozone-platform=qnx-screen
//     Uses libscreen directly for native compositing
//     Best performance, full Adreno 330 utilization
//     Requires custom Ozone platform implementation

#ifndef GPU_QNX_GPU_QNX_H_
#define GPU_QNX_GPU_QNX_H_

// EGL/GLES library names on QNX
#define QNX_EGL_LIBRARY "libEGL.so"
#define QNX_GLES2_LIBRARY "libGLESv2.so"
#define QNX_SCREEN_LIBRARY "libscreen.so"

// Adreno 330 capabilities
#define QNX_GPU_MAX_TEXTURE_SIZE 4096
#define QNX_GPU_SUPPORTS_GLES3 1
#define QNX_GPU_SUPPORTS_NEON 1

// Display properties for BlackBerry Passport
#define QNX_DISPLAY_WIDTH 1440
#define QNX_DISPLAY_HEIGHT 1440
#define QNX_DISPLAY_DPI 453

namespace gpu {
namespace qnx {

// GPU feature flags for QNX/Adreno 330
struct GpuCapabilities {
    bool has_egl;
    bool has_gles2;
    bool has_gles3;
    bool has_neon;
    int max_texture_size;
    int display_width;
    int display_height;
    int display_dpi;
};

// Detect GPU capabilities at runtime
inline GpuCapabilities DetectCapabilities() {
    GpuCapabilities caps = {};
    caps.has_egl = true;       // EGL always available on Passport
    caps.has_gles2 = true;     // GLES2 always available
    caps.has_gles3 = true;     // Adreno 330 supports GLES3
    caps.has_neon = true;      // Krait 400 has NEON
    caps.max_texture_size = QNX_GPU_MAX_TEXTURE_SIZE;
    caps.display_width = QNX_DISPLAY_WIDTH;
    caps.display_height = QNX_DISPLAY_HEIGHT;
    caps.display_dpi = QNX_DISPLAY_DPI;
    return caps;
}

// Load EGL/GLES2 libraries dynamically (no link-time dependency)
bool InitializeGpuLibraries();

// Initialize EGL display and choose config
// out_display and out_config are opaque EGL handles
bool InitializeEglDisplay(void** out_display, void** out_config);

// Print GPU vendor/renderer/version info
void PrintGpuInfo();

// Release GPU resources
void ShutdownGpu();

}  // namespace qnx
}  // namespace gpu

#endif  // GPU_QNX_GPU_QNX_H_
