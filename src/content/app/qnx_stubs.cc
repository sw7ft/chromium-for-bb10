// QNX platform stubs for content_shell
// Provides stub implementations for Linux-specific symbols that are
// referenced but not available on QNX.

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/scoped_file.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_QNX)

// === TLS runtime support ===
// QNX's dynamic linker doesn't export __tls_get_addr like glibc's ld-linux.so.
// Provide a weak stub. Actual TLS should work via compiler-generated code.
extern "C" __attribute__((weak))
void* __tls_get_addr(void* ti) {
  return nullptr;
}

// === partition_alloc stack trace ===
namespace partition_alloc::internal::base::debug {
size_t CollectStackTrace(const void** trace, size_t count) {
  return 0;
}
}  // namespace partition_alloc::internal::base::debug

// === base process memory ===
namespace base {
void EnableTerminationOnOutOfMemory() {}
void EnableTerminationOnHeapCorruption() {}
}  // namespace base

// === base stack canary ===
namespace base {
void ResetStackCanaryIfPossible() {}
void SetStackSmashingEmitsDebugMessage() {}
}  // namespace base

// === content zygote (Linux-only, not needed on QNX) ===
namespace content {

class ZygoteForkDelegate;

int ZygoteMain(
    std::vector<std::unique_ptr<ZygoteForkDelegate>> delegates) {
  return 0;
}

}  // namespace content

// === content sandbox (Linux-only) ===
namespace content {

class SandboxHostLinux {
 public:
  static SandboxHostLinux* GetInstance();
  void Init();
 private:
  SandboxHostLinux() = default;
};

SandboxHostLinux* SandboxHostLinux::GetInstance() {
  static SandboxHostLinux instance;
  return &instance;
}

void SandboxHostLinux::Init() {}

int GetSandboxFD() {
  return -1;
}

}  // namespace content

// === content zygote host (Linux-only) ===
namespace content {

class ZygoteHostImpl {
 public:
  static ZygoteHostImpl* GetInstance();
  void Init(const base::CommandLine& cmd);
  void SetRendererSandboxStatus(int status);
  bool LaunchZygote(base::CommandLine* cmd,
                    base::ScopedFD* control_fd,
                    std::vector<std::pair<int, int>> fds);
 private:
  ZygoteHostImpl() = default;
};

ZygoteHostImpl* ZygoteHostImpl::GetInstance() {
  static ZygoteHostImpl instance;
  return &instance;
}

void ZygoteHostImpl::Init(const base::CommandLine&) {}
void ZygoteHostImpl::SetRendererSandboxStatus(int) {}
bool ZygoteHostImpl::LaunchZygote(base::CommandLine*,
                                   base::ScopedFD*,
                                   std::vector<std::pair<int, int>>) {
  return false;
}

class ZygoteCommunication {
 public:
  int GetSandboxStatus();
};

int ZygoteCommunication::GetSandboxStatus() {
  return 0;
}

}  // namespace content

#endif  // BUILDFLAG(IS_QNX)
