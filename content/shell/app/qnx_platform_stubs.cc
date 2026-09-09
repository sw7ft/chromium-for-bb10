// QNX platform stubs for content_shell
// Provides stub/no-op implementations for platform-specific symbols.

#include "build/build_config.h"

#if BUILDFLAG(IS_QNX)

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <ostream>
#include <string>
#include <typeinfo>
#include <unordered_set>
#include <vector>

#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/containers/flat_set.h"
#include "base/debug/elf_reader.h"
#include "base/debug/stack_trace.h"
#include "base/files/file_path.h"
#include "base/files/file_path_watcher.h"
#include "base/files/scoped_file.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/no_destructor.h"
#include "base/process/memory.h"
#include "base/process/process.h"
#include "base/process/process_handle.h"
#include "base/process/process_metrics.h"
#include "base/system/sys_info.h"
#include "base/threading/platform_thread.h"
#include "base/threading/platform_thread_internal_posix.h"
#include "base/time/time.h"
#include "ui/events/keycodes/dom/dom_code.h"

// =====================================================================
// Low-level runtime stubs (extern "C")
// =====================================================================

extern "C" {

// Stack-protector guard, defined here as a POINTER to the canary.
//
// Under ThinLTO, LLVM's ARM backend lowers every canary access to an
// indirect pattern -- movw/movt of &__stack_chk_guard, then TWO loads
// (slot -> pointer -> canary) -- regardless of relocation model or
// dso_local. Without a definition in the executable, lld resolved the
// libc-defined guard with an R_ARM_COPY relocation, so the slot held the
// canary VALUE and the second load dereferenced random canary bytes:
// build 94 crashed at boot, SIGSEGV in the interposed strlen below (the
// first protected function the dynamic linker calls), fault address
// changing every run.
//
// Making the slot actually contain a pointer satisfies the indirect
// (LTO) pattern, and the classic GOT pattern in non-LTO objects and
// shared libs stays self-consistent too: each function's prologue and
// epilogue load the same expression, whatever it dereferences to.
static unsigned long berry_stack_canary = 0x000aff0d;  // NUL/LF/EOF bytes
__attribute__((visibility("default"), used))
unsigned long* __stack_chk_guard = &berry_stack_canary;

// Safe strlen: QNX's __strlen_isr crashes on NULL.  We mark these with
// default visibility so they appear in the dynamic symbol table and override
// the versions in ldqnx.so.2 / libc.so.3 for ALL callers including shared
// libraries like libstdc++.so.6.
__attribute__((visibility("default"), used, noinline))
size_t strlen(const char* s) {
  if (__builtin_expect(s == nullptr, 0)) {
    const char msg[] = "QNX_STUB: strlen(NULL) from=0x";
    write(2, msg, sizeof(msg) - 1);
    char hex[9];
    unsigned long ra = (unsigned long)__builtin_return_address(0);
    for (int i = 7; i >= 0; --i) {
      int d = ra & 0xf;
      hex[i] = d < 10 ? '0' + d : 'a' + d - 10;
      ra >>= 4;
    }
    hex[8] = '\n';
    write(2, hex, 9);
    return 0;
  }
  const char* p = s;
  while (*p) p++;
  return p - s;
}
// QNX may also resolve to this ISR-optimized name
__attribute__((visibility("default"), used, noinline))
size_t __strlen_isr(const char* s) { return strlen(s); }

// Strong strncpy: QNX 8.0's <string_chk.h> (_FORTIFY_SOURCE) emits a weak
// out-of-line strncpy under clang that recurses into itself, which the
// optimizer collapses to an infinite loop (`b.n .`). Any TU built with
// fortify can inject that trap into the link, hijacking every strncpy call
// (observed: browser + renderer threads spinning at 100% CPU inside
// breakpad's SetKeyValue). This strong definition wins over the weak trap.
// Fortify is also disabled for QNX in build/config/compiler/BUILD.gn.
// (Defined under a different C++ name with an asm label, because the fortify
// header already declares strncpy as always_inline in this TU.)
char* qnx_strncpy_impl(char* dst, const char* src, size_t n) __asm__("strncpy");
__attribute__((visibility("default"), used, noinline))
char* qnx_strncpy_impl(char* dst, const char* src, size_t n) {
  size_t i = 0;
  for (; i < n && src[i] != '\0'; ++i)
    dst[i] = src[i];
  for (; i < n; ++i)
    dst[i] = '\0';
  return dst;
}

// QNX diagnostic: interpose std::__throw_system_error(int) to capture the
// error code and the exact (inline) caller before the exception machinery
// runs. Forwards to the real libstdc++ implementation.
#if 0  // Disabled after QNX page-load bring-up.
typedef void (*qnx_tse_t)(int);
__attribute__((visibility("default"), used, noreturn))
void _ZSt20__throw_system_errori(int err) {
  char b[160];
  int n = snprintf(b, sizeof(b), "QNX:TSE err=%d from=%p tid=%d\n", err,
                   __builtin_return_address(0), (int)pthread_self());
  if (n > 0) write(2, b, n);
  static qnx_tse_t real_tse = nullptr;
  if (!real_tse)
    real_tse = (qnx_tse_t)dlsym(RTLD_NEXT, "_ZSt20__throw_system_errori");
  if (real_tse) real_tse(err);
  abort();
}
#endif

#if 0  // Disabled after QNX page-load bring-up.
// QNX diagnostic: interpose __cxa_throw to find code throwing C++ exceptions
// in a loop (observed: threads pegged in the unwinder's dl/phdr scans during
// page load). Logs the first throws and every 1000th, then forwards to the
// real libstdc++ implementation.
typedef void (*qnx_cxa_throw_t)(void*, void*, void (*)(void*));
__attribute__((visibility("default"), used))
void __cxa_throw(void* ex, void* tinfo, void (*dest)(void*)) {
  static qnx_cxa_throw_t real_throw = nullptr;
  static unsigned long count = 0;
  unsigned long c = __atomic_add_fetch(&count, 1, __ATOMIC_RELAXED);
  if (c <= 20 || (c % 1000) == 0) {
    const char* name =
        tinfo ? reinterpret_cast<const std::type_info*>(tinfo)->name() : "?";
    char b[256];
    int n = snprintf(b, sizeof(b),
                     "QNX:THROW #%lu type=%s from=%p tid=%d errno=%d\n", c,
                     name, __builtin_return_address(0), (int)pthread_self(),
                     errno);
    if (n > 0) write(2, b, n);
    void* sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    uintptr_t* sp_ptr = (uintptr_t*)sp;
    int found = 0;
    for (int i = 0; i < 512 && found < 24; i++) {
      uintptr_t word = sp_ptr[i];
      if (word > 0x800000 && word < 0x8000000) {
        n = snprintf(b, sizeof(b), "QNX:THROWSTK[%d]=0x%lx\n", i,
                     (unsigned long)word);
        if (n > 0) write(2, b, n);
        found++;
      }
    }
  }
  if (!real_throw)
    real_throw = (qnx_cxa_throw_t)dlsym(RTLD_NEXT, "__cxa_throw");
  if (real_throw) real_throw(ex, tinfo, dest);
  abort();  // __cxa_throw must not return.
}
#endif

// With -femulated-tls, Clang uses __emutls_get_address (from libgcc_eh.a)
// instead of native ARM TLS.  These stubs are kept as safe fallbacks in case
// any pre-compiled object still references the native TLS symbols.
struct tls_index {
  unsigned long int ti_module;
  unsigned long int ti_offset;
};
__attribute__((weak)) void* __tls_get_addr(struct tls_index*) {
  return nullptr;
}
__attribute__((weak)) void* __aeabi_read_tp() {
  return nullptr;
}

// __atomic_is_lock_free is a compiler builtin; provide via libatomic if needed

__attribute__((weak)) void OPENSSL_cpuid_setup(void) {}

// Fontconfig stubs
void* FcFontList(void* config, void* p, void* os) { return 0; }
void FcFontSetDestroy(void* fs) {}
void* FcObjectSetBuild(const char* first, ...) { return 0; }
void FcObjectSetDestroy(void* os) {}
int FcPatternAddBool(void* p, const char* object, int b) { return 0; }
int FcPatternAddString(void* p, const char* object, const unsigned char* s) { return 0; }
void* FcPatternCreate(void) { return 0; }
void FcPatternDestroy(void* p) {}
int FcPatternGetString(const void* p, const char* object, int n, unsigned char** s) { return 1; }

}  // extern "C"

// =====================================================================
// ANGLE stubs
// =====================================================================
namespace angle {
void BreakDebugger() { __builtin_trap(); }
bool IsDebuggerAttached() { return false; }
}

// =====================================================================
// partition_alloc
// =====================================================================
namespace partition_alloc::internal::base::debug {
size_t CollectStackTrace(const void** trace, size_t count) { return 0; }
}

// =====================================================================
// base:: stubs
// =====================================================================
namespace base {

void EnableTerminationOnOutOfMemory() {}
void EnableTerminationOnHeapCorruption() {}
void ResetStackCanaryIfPossible() {}
void SetStackSmashingEmitsDebugMessage() {}
void InitThreading() {}
void TerminateOnThread() {}

void PlatformThreadBase::SetName(const std::string& name) {}

size_t GetDefaultThreadStackSize(const pthread_attr_t& attributes) {
  // Returning 0 means "use the platform default stack". On Linux that is ~8MB,
  // but QNX's default pthread stack is only a few hundred KB - far too small for
  // Chromium's deep compositor/Skia raster and V8 call chains, which overflow it
  // and crash with SIGSEGV/SIGBUS at an address next to the stack pointer a few
  // seconds after a page starts rendering. Give every Chromium-created thread an
  // 8MB stack to match Linux. The reservation is virtual (committed on demand),
  // so this is cheap on the 32-bit address space.
  return 8 * 1024 * 1024;
}

ProcessId GetParentProcessId(ProcessHandle process) { return 1; }

Time Process::CreationTime() const { return Time(); }
Process::Priority Process::GetPriority() const { return Priority::kUserBlocking; }

std::unique_ptr<ProcessMetrics> ProcessMetrics::CreateProcessMetrics(
    ProcessHandle process) { return nullptr; }
TimeDelta ProcessMetrics::GetCumulativeCPUUsage() { return TimeDelta(); }

uint64_t SysInfo::AmountOfPhysicalMemoryImpl() { return 2ULL * 1024 * 1024 * 1024; }
uint64_t SysInfo::AmountOfAvailablePhysicalMemoryImpl() { return 1ULL * 1024 * 1024 * 1024; }

bool UncheckedMalloc(size_t size, void** result) {
  *result = malloc(size);
  return *result != nullptr;
}
void UncheckedFree(void* ptr) { free(ptr); }

FilePathWatcher::FilePathWatcher() = default;

bool PathProviderPosix(int key, FilePath* result) {
  switch (key) {
    case FILE_EXE:
    case FILE_MODULE: {
      char* cpath = getenv("CHROME_EXE_PATH");
      *result = FilePath(cpath ? cpath : "/tmp/berry-deploy/content_shell");
      return true;
    }
    case DIR_SRC_TEST_DATA_ROOT:
    case DIR_USER_DESKTOP:
    case DIR_CACHE:
      *result = FilePath("/tmp");
      return true;
  }
  return false;
}

namespace debug {
span<const Phdr> GetElfProgramHeaders(const void* elf_mapped_base) { return {}; }
size_t GetRelocationOffset(const void* elf_mapped_base) { return 0; }
size_t ReadElfBuildId(const void* elf_mapped_base, bool uppercase,
                      ElfBuildIdBuffer build_id) { return 0; }
void StackTrace::OutputToStreamWithPrefix(std::ostream* os,
                                          const char* prefix_string) const {
  if (os) *os << "(stack trace unavailable on QNX)\n";
}
}  // namespace debug

namespace internal {
const ThreadTypeToNiceValuePair kThreadTypeToNiceValueMap[7] = {
    {ThreadType::kBackground, 10},
    {ThreadType::kUtility, 2},
    {ThreadType::kResourceEfficient, 1},
    {ThreadType::kDefault, 0},
    {ThreadType::kCompositing, -1},
    {ThreadType::kDisplayCritical, -8},
    {ThreadType::kRealtimeAudio, -10},
};
bool CanSetThreadTypeToRealtimeAudio() { return false; }
}  // namespace internal

}  // namespace base

// =====================================================================
// content:: stubs
// =====================================================================

// --- SandboxHostLinux ---
#include "content/browser/sandbox_host_linux.h"
namespace content {
// Private constructor - befriend via NoDestructor
SandboxHostLinux::SandboxHostLinux() = default;
SandboxHostLinux* SandboxHostLinux::GetInstance() {
  static base::NoDestructor<SandboxHostLinux> instance;
  return instance.get();
}
void SandboxHostLinux::Init() {}
}  // namespace content

// --- ZygoteHostImpl ---
#include "content/browser/zygote_host/zygote_host_impl_linux.h"
namespace content {
ZygoteHostImpl::ZygoteHostImpl() : renderer_sandbox_status_(-1) {}
ZygoteHostImpl::~ZygoteHostImpl() = default;
ZygoteHostImpl* ZygoteHostImpl::GetInstance() {
  static ZygoteHostImpl instance;
  return &instance;
}
void ZygoteHostImpl::Init(const base::CommandLine& cmd_line) {}
void ZygoteHostImpl::SetRendererSandboxStatus(int status) {
  renderer_sandbox_status_ = status;
}
int ZygoteHostImpl::GetRendererSandboxStatus() { return renderer_sandbox_status_; }
pid_t ZygoteHostImpl::LaunchZygote(
    base::CommandLine* cmd_line, base::ScopedFD* control_fd,
    base::FileHandleMappingVector fds) { return -1; }
bool ZygoteHostImpl::IsZygotePid(pid_t pid) { return false; }
void ZygoteHostImpl::AdjustRendererOOMScore(base::ProcessHandle, int) {}
}  // namespace content

// --- ZygoteCommunication ---
#include "content/common/zygote/zygote_communication_linux.h"
namespace content {
ZygoteCommunication::ZygoteCommunication(ZygoteType type) : type_(type) {}
ZygoteCommunication::~ZygoteCommunication() = default;
void ZygoteCommunication::Init(
    base::OnceCallback<pid_t(base::CommandLine*, base::ScopedFD*)> cb) {}
int ZygoteCommunication::GetSandboxStatus() { return 0; }
}  // namespace content

// --- GetSandboxFD ---
namespace content {
int GetSandboxFD() { return -1; }
}  // namespace content

// --- ZygoteMain ---
#include "content/public/common/zygote/zygote_handle.h"
namespace content {
class ZygoteForkDelegate;
int ZygoteMain(std::vector<std::unique_ptr<ZygoteForkDelegate>> delegates) { return 0; }
}  // namespace content

// --- ChildProcessLauncherHelper platform methods are in
// child_process_launcher_helper_qnx.cc (content/browser) for QNX.

// --- NativeEventObserver ---
// Defined with matching ABI but no base class for QNX
namespace content::responsiveness {
class NativeEventObserver {
 public:
  void RegisterObserver();
  void DeregisterObserver();
};
void NativeEventObserver::RegisterObserver() {}
void NativeEventObserver::DeregisterObserver() {}
}  // namespace content::responsiveness

// --- RendererMainPlatformDelegate ---
namespace content {
class MainFunctionParams;
class RendererMainPlatformDelegate {
 public:
  explicit RendererMainPlatformDelegate(const MainFunctionParams&);
  ~RendererMainPlatformDelegate();
  void PlatformInitialize();
  void PlatformUninitialize();
  void EnableSandbox();
};
RendererMainPlatformDelegate::RendererMainPlatformDelegate(const MainFunctionParams&) {}
RendererMainPlatformDelegate::~RendererMainPlatformDelegate() = default;
void RendererMainPlatformDelegate::PlatformInitialize() {}
void RendererMainPlatformDelegate::PlatformUninitialize() {}
void RendererMainPlatformDelegate::EnableSandbox() {}
}  // namespace content

// --- TtsPlatformImpl ---
namespace content {
class TtsPlatformImpl {
 public:
  static TtsPlatformImpl* GetInstance();
};
TtsPlatformImpl* TtsPlatformImpl::GetInstance() {
  static TtsPlatformImpl instance;
  return &instance;
}
}  // namespace content

// =====================================================================
// views stubs
// =====================================================================
#include "ui/views/widget/desktop_aura/desktop_window_tree_host.h"
namespace views {
DesktopWindowTreeHost* DesktopWindowTreeHost::Create(
    internal::NativeWidgetDelegate* native_widget_delegate,
    DesktopNativeWidgetAura* desktop_native_widget_aura) { return nullptr; }
}  // namespace views

#include "ui/views/controls/menu/menu_config.h"
namespace views {
void MenuConfig::Init() {}
void MenuConfig::InitPlatformCR2023() {}
}  // namespace views

#include "ui/display/screen.h"
namespace views {
std::unique_ptr<display::Screen> CreateDesktopScreen() { return nullptr; }
}  // namespace views

// =====================================================================
// ui stubs
// =====================================================================
#include "ui/events/keyboard_hook.h"
namespace ui {
std::unique_ptr<KeyboardHook> KeyboardHook::CreateModifierKeyboardHook(
    std::optional<base::flat_set<DomCode>> dom_codes,
    gfx::AcceleratedWidget accelerated_widget,
    KeyEventCallback callback) { return nullptr; }
int CalculateIdleTime() { return 0; }
bool CheckIdleStateIsLocked() { return false; }
}  // namespace ui

// QNX has no platform clipboard backend; use Chromium's in-memory clipboard so
// selection/copy paths (e.g. updating the selection buffer on click) work and
// don't CHECK-fail on a null clipboard.
#include "ui/base/clipboard/clipboard.h"
#include "ui/base/clipboard/clipboard_non_backed.h"
namespace ui {
Clipboard* Clipboard::Create() { return new ClipboardNonBacked; }
}  // namespace ui

#include "ui/shell_dialogs/select_file_dialog.h"
namespace ui {
SelectFileDialog* CreateSelectFileDialog(
    SelectFileDialog::Listener* listener,
    std::unique_ptr<SelectFilePolicy> policy) { return nullptr; }
}  // namespace ui

// OSExchangeDataProviderNonBacked - included in build via ui/base/BUILD.gn

#include "ui/base/resource/resource_bundle.h"
namespace ui {
gfx::Image& ResourceBundle::GetNativeImageNamed(int resource_id) {
  return GetImageNamed(resource_id);
}
}  // namespace ui

// =====================================================================
// GL/GPU stubs
// =====================================================================
// NOTE: The gl::init free functions (GetAllowedGLImplementations,
// CreateGLContext, CreateOffscreenGLSurfaceWithFormat, InitializeGLOneOffPlatform,
// InitializeStaticGLBindings, ShutdownGLPlatform, ...) are intentionally NOT
// stubbed here anymore. The real Ozone implementations in libgl_init.a
// (gl_factory_ozone.cc + gl_initializer_ozone.cc) provide them and route to the
// qnx_screen GLOzone (native EGL/GLES2 on the Adreno). Stubbing them shadowed the
// archive members and forced GL = none (empty allowed-impl list), which broke
// GPU compositing.

// On-screen GPU output surface. The upstream Ozone implementation lives in
// gpu/ipc/service/image_transport_surface_linux.cc, but that file is only built
// for is_linux || is_chromeos (not QNX), so we provide the same behavior here.
// Returning nullptr (the old stub) forced every GPU command buffer / Viz output
// surface to fall back to offscreen + software blit to the QNX window, which is
// why nothing ever reached our QnxScreenGLOzoneEGL::CreateViewGLSurface and the
// browser stayed on software compositing. Route to the real Ozone view surface.
#include "gpu/ipc/service/image_transport_surface.h"
#include "gpu/ipc/service/pass_through_image_transport_surface.h"
#include "ui/gl/init/gl_factory.h"
namespace gpu {
scoped_refptr<gl::GLSurface> ImageTransportSurface::CreateNativeGLSurface(
    gl::GLDisplay* display,
    base::WeakPtr<ImageTransportSurfaceDelegate> delegate,
    gpu::SurfaceHandle surface_handle,
    gl::GLSurfaceFormat format) {
  scoped_refptr<gl::GLSurface> surface =
      gl::init::CreateViewGLSurface(display, surface_handle);
  if (!surface)
    return surface;
  return base::MakeRefCounted<PassThroughImageTransportSurface>(
      delegate, surface.get(), /*override_vsync_for_multi_window_swap=*/false);
}
// QNX has no surfaceless/overlay presenter; returning null makes Viz fall back
// to the on-screen GLSurface path above (SkiaOutputDeviceGL).
scoped_refptr<gl::Presenter> ImageTransportSurface::CreatePresenter(
    gl::GLDisplay*, base::WeakPtr<ImageTransportSurfaceDelegate>,
    SurfaceHandle, gl::GLSurfaceFormat) { return nullptr; }
}  // namespace gpu

#include "gpu/config/gpu_info.h"
namespace gpu {
bool CollectBasicGraphicsInfo(GPUInfo* gpu_info) { return false; }
bool CollectContextGraphicsInfo(GPUInfo* gpu_info) { return false; }
}  // namespace gpu

// =====================================================================
// gfx stubs
// =====================================================================
#include "ui/gfx/font.h"
#include "ui/gfx/font_render_params.h"
namespace gfx {
bool GetFallbackFont(const Font& font, const std::string& locale,
                     std::basic_string_view<char16_t> text, Font* result) { return false; }
std::vector<Font> GetFallbackFonts(const Font& font) { return {}; }
FontRenderParams GetFontRenderParams(const FontRenderParamsQuery& query,
                                     std::string* family_out) { return FontRenderParams(); }
float GetFontRenderParamsDeviceScaleFactor() { return 1.0f; }
}  // namespace gfx

// =====================================================================
// Blink font/theme stubs
// =====================================================================
#include "third_party/blink/renderer/platform/fonts/font_cache.h"
namespace blink {
const AtomicString& FontCache::SystemFontFamily() {
  static const AtomicString family("sans-serif");
  return family;
}
scoped_refptr<SimpleFontData> FontCache::PlatformFallbackFontForCharacter(
    const FontDescription& description,
    UChar32,
    const SimpleFontData*,
    FontFallbackPriority) {
  return FontCache::Get().GetLastResortFallbackFont(description);
}
}  // namespace blink

#include "third_party/blink/renderer/core/layout/layout_theme_default.h"
#include "third_party/blink/renderer/platform/wtf/std_lib_extras.h"
namespace blink {
namespace {
class LayoutThemeQnx final : public LayoutThemeDefault {
 public:
  static scoped_refptr<LayoutTheme> Create() {
    return base::AdoptRef(new LayoutThemeQnx());
  }
};
}  // namespace
LayoutTheme& LayoutTheme::NativeTheme() {
  DEFINE_STATIC_REF(LayoutTheme, layout_theme, (LayoutThemeQnx::Create()));
  return *layout_theme;
}
}  // namespace blink

// Skia font manager — QNX has no fontconfig. BB10 devices ship TrueType fonts
// under /usr/fonts/font_repository (DejaVu, Monotype: Verdana/Times/Arial/...),
// so build a directory-backed font manager from them. The path is overridable
// via QNX_FONT_DIR. If no fonts are found we fall back to the empty font mgr so
// FontCache never hits CrashWithFontInfo when fallback runs.
#include <cstdlib>

#include "third_party/skia/include/core/SkFontMgr.h"
#include "third_party/skia/include/core/SkTypeface.h"
#include "third_party/skia/include/ports/SkFontConfigInterface.h"
#include "third_party/skia/include/ports/SkFontMgr_directory.h"
#include "third_party/skia/include/ports/SkFontMgr_empty.h"

namespace {

class QnxFontConfigInterface : public SkFontConfigInterface {
 public:
  bool matchFamilyName(const char*,
                       SkFontStyle,
                       FontIdentity*,
                       SkString*,
                       SkFontStyle*) override {
    return false;
  }

  SkStreamAsset* openStream(const FontIdentity&) override { return nullptr; }

  sk_sp<SkTypeface> makeTypeface(const FontIdentity&) override {
    return nullptr;
  }
};

}  // namespace

sk_sp<SkFontConfigInterface> SkFontConfigInterface::RefGlobal() {
  static sk_sp<SkFontConfigInterface> g = sk_make_sp<QnxFontConfigInterface>();
  return g;
}

namespace skia {
sk_sp<SkFontMgr> CreateDefaultSkFontMgr() {
  const char* font_dir = getenv("QNX_FONT_DIR");
  if (!font_dir || !font_dir[0])
    font_dir = "/usr/fonts/font_repository";
  sk_sp<SkFontMgr> mgr = SkFontMgr_New_Custom_Directory(font_dir);
  if (mgr && mgr->countFamilies() > 0)
    return mgr;
  return SkFontMgr_New_Custom_Empty();
}
}  // namespace skia

// =====================================================================
// printing
// =====================================================================
namespace printing {
class PrintingContext {
 public:
  class Delegate;
  enum class ProcessBehavior;
  static std::unique_ptr<PrintingContext> CreateImpl(Delegate*, ProcessBehavior);
};
std::unique_ptr<PrintingContext> PrintingContext::CreateImpl(Delegate*, ProcessBehavior) {
  return nullptr;
}
}  // namespace printing

// =====================================================================
// media
// =====================================================================
// NOTE: media::CreateAudioManager is intentionally NOT stubbed here. The real
// implementation lives in media/audio/openal/audio_manager_openal.cc and returns
// an OpenAL-backed AudioManager (with QSA microphone capture). A stub returning
// FakeAudioManager here would shadow it at link time (this object is linked
// directly into the executable, overriding the media archive), leaving the app
// with no real audio output and no microphone input.

// =====================================================================
// net - avoid including full cert headers to prevent incomplete type issues
// =====================================================================
#include "net/base/platform_mime_util.h"
namespace net {
bool PlatformMimeUtil::GetPlatformMimeTypeFromExtension(
    const std::string& ext, std::string* mime_type) const { return false; }
bool PlatformMimeUtil::GetPlatformPreferredExtensionForMimeType(
    const std::string& mime_type, std::string* extension) const { return false; }
void PlatformMimeUtil::GetPlatformExtensionsForMimeType(
    const std::string& mime_type,
    std::unordered_set<std::string>* extensions) const {}
}  // namespace net

// CertVerifyProc::CreateSystemVerifyProc - use full includes
#include "net/cert/cert_verify_proc.h"
#include "net/cert/cert_net_fetcher.h"
#include "net/cert/crl_set.h"
namespace net {
scoped_refptr<CertVerifyProc> CertVerifyProc::CreateSystemVerifyProc(
    scoped_refptr<CertNetFetcher> cert_net_fetcher,
    scoped_refptr<CRLSet> crl_set) { return nullptr; }
}  // namespace net

// =====================================================================
// device
// =====================================================================
#include "services/device/time_zone_monitor/time_zone_monitor.h"
namespace device {
std::unique_ptr<TimeZoneMonitor> TimeZoneMonitor::Create(
    scoped_refptr<base::SequencedTaskRunner> file_task_runner) { return nullptr; }
}  // namespace device

namespace device {
class GeolocationManager;
class LocationProvider;
std::unique_ptr<LocationProvider> NewSystemLocationProvider(
    scoped_refptr<base::SingleThreadTaskRunner>, GeolocationManager*) { return nullptr; }
}  // namespace device

// =====================================================================
// memory_instrumentation
// =====================================================================
#include "services/resource_coordinator/public/cpp/memory_instrumentation/os_metrics.h"
namespace memory_instrumentation {
bool OSMetrics::FillOSMemoryDump(base::ProcessId pid,
                                 mojom::RawOSMemDump* dump) { return false; }
std::vector<mojom::VmRegionPtr> OSMetrics::GetProcessMemoryMaps(
    base::ProcessId pid) { return {}; }
}  // namespace memory_instrumentation

// crash reporter stubs are now in crashpad.cc (guarded by IS_QNX)

// =====================================================================
// V8 platform stubs - must match v8/src/base/platform/platform.h ABI
// =====================================================================
namespace v8::base {
class TimezoneCache;
class OS {
 public:
  struct SharedLibraryAddress {
    std::string library_path;
    uintptr_t start;
    uintptr_t end;
    intptr_t aslr_slide;
    SharedLibraryAddress(const std::string& p, uintptr_t s, uintptr_t e, intptr_t sl)
        : library_path(p), start(s), end(e), aslr_slide(sl) {}
  };
  static void AdjustSchedulingParams();
  static bool ArmUsingHardFloat();
  static TimezoneCache* CreateTimezoneCache();
  static std::vector<SharedLibraryAddress> GetSharedLibraryAddresses();
  static void SignalCodeMovingGC();
};
void OS::AdjustSchedulingParams() {}
bool OS::ArmUsingHardFloat() { return false; }
TimezoneCache* OS::CreateTimezoneCache() { return nullptr; }
std::vector<OS::SharedLibraryAddress> OS::GetSharedLibraryAddresses() { return {}; }
void OS::SignalCodeMovingGC() {}
}  // namespace v8::base

#endif  // BUILDFLAG(IS_QNX)
