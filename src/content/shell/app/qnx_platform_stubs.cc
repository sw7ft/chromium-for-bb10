// QNX platform stubs for content_shell
// Provides stub/no-op implementations for platform-specific symbols.

#include "build/build_config.h"

#if BUILDFLAG(IS_QNX)

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <ostream>
#include <string>
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

size_t GetDefaultThreadStackSize(const pthread_attr_t& attributes) { return 0; }

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

// --- ChildProcessLauncherHelper platform methods ---
#include "content/browser/child_process_launcher_helper.h"
namespace content::internal {
void ChildProcessLauncherHelper::SetProcessPriorityOnLauncherThread(
    base::Process process, base::Process::Priority priority) {}
void ChildProcessLauncherHelper::ForceNormalProcessTerminationSync(
    ChildProcessLauncherHelper::Process process) {}
void ChildProcessLauncherHelper::BeforeLaunchOnClientThread() {}
bool ChildProcessLauncherHelper::BeforeLaunchOnLauncherThread(
    PosixFileDescriptorInfo& files_to_register,
    base::LaunchOptions* options) { return true; }
void ChildProcessLauncherHelper::AfterLaunchOnLauncherThread(
    const ChildProcessLauncherHelper::Process& process,
    const base::LaunchOptions* options) {}
absl::optional<mojo::NamedPlatformChannel>
ChildProcessLauncherHelper::CreateNamedPlatformChannelOnLauncherThread() {
  return absl::nullopt;
}
std::unique_ptr<PosixFileDescriptorInfo>
ChildProcessLauncherHelper::GetFilesToMap() { return nullptr; }
ChildProcessTerminationInfo
ChildProcessLauncherHelper::GetTerminationInfo(
    const ChildProcessLauncherHelper::Process& process, bool known_dead) { return {}; }
bool ChildProcessLauncherHelper::IsUsingLaunchOptions() { return false; }
ChildProcessLauncherHelper::Process
ChildProcessLauncherHelper::LaunchProcessOnLauncherThread(
    const base::LaunchOptions* options,
    std::unique_ptr<PosixFileDescriptorInfo> files_to_register,
    bool* is_synchronous_launch, int* launch_result) {
  *is_synchronous_launch = false;
  *launch_result = 1;
  return Process();
}
bool ChildProcessLauncherHelper::TerminateProcess(
    const base::Process& process, int exit_code) { return false; }
}  // namespace content::internal

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

#include "ui/base/clipboard/clipboard.h"
namespace ui {
Clipboard* Clipboard::Create() { return nullptr; }
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
#include "ui/gl/gl_context.h"
#include "ui/gl/gl_surface.h"
#include "ui/gl/gl_display.h"
#include "ui/gl/gl_implementation.h"
namespace gl::init {
scoped_refptr<GLContext> CreateGLContext(GLShareGroup* share_group,
                                         GLSurface* compatible_surface,
                                         const GLContextAttribs& attribs) { return nullptr; }
scoped_refptr<GLSurface> CreateOffscreenGLSurfaceWithFormat(
    GLDisplay* display, const gfx::Size& size, GLSurfaceFormat format) { return nullptr; }
std::vector<GLImplementationParts> GetAllowedGLImplementations() { return {}; }
bool GetGLWindowSystemBindingInfo(const GLVersionInfo& gl_info,
                                  GLWindowSystemBindingInfo* info) { return false; }
bool InitializeExtensionSettingsOneOffPlatform(GLDisplay* display) { return true; }
GLDisplay* InitializeGLOneOffPlatform(gl::GpuPreference pref) { return nullptr; }
bool InitializeStaticGLBindings(GLImplementationParts impl) { return false; }
void SetDisabledExtensionsPlatform(const std::string& disabled) {}
void ShutdownGLPlatform(GLDisplay* display) {}
}  // namespace gl::init

#include "gpu/ipc/service/image_transport_surface.h"
namespace gpu {
scoped_refptr<gl::GLSurface> ImageTransportSurface::CreateNativeGLSurface(
    gl::GLDisplay*, base::WeakPtr<ImageTransportSurfaceDelegate>,
    gpu::SurfaceHandle, gl::GLSurfaceFormat) { return nullptr; }
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
    const FontDescription&, UChar32, const SimpleFontData*,
    FontFallbackPriority) { return nullptr; }
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

// Skia font manager
#include "third_party/skia/include/core/SkFontMgr.h"
namespace skia {
sk_sp<SkFontMgr> CreateDefaultSkFontMgr() { return nullptr; }
}  // namespace skia

// SkFontConfigInterface
class SkFontConfigInterface : public SkRefCnt {
 public:
  static sk_sp<SkFontConfigInterface> RefGlobal();
};
sk_sp<SkFontConfigInterface> SkFontConfigInterface::RefGlobal() { return nullptr; }

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
#include "media/audio/audio_manager.h"
#include "media/audio/fake_audio_manager.h"
namespace media {
std::unique_ptr<AudioManager> CreateAudioManager(
    std::unique_ptr<AudioThread> audio_thread,
    AudioLogFactory* audio_log_factory) {
  return std::make_unique<FakeAudioManager>(std::move(audio_thread),
                                            audio_log_factory);
}
}  // namespace media

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
