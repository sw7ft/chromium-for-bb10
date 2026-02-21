// Copyright 2025 SW7FT. All rights reserved.
// Sandbox stub for QNX - Chromium sandbox is disabled on QNX
//
// Chromium's sandbox uses Linux-specific features:
//   - seccomp-bpf (system call filtering)
//   - namespaces (user, PID, network)
//   - chroot
//   - setuid sandbox
//
// None of these are available on QNX. For a single-user embedded device
// like the BlackBerry Passport, the sandbox is not essential.
//
// This stub provides no-op implementations of the sandbox interface
// so that Chromium can build and run without sandboxing.

#ifndef SANDBOX_QNX_SANDBOX_QNX_H_
#define SANDBOX_QNX_SANDBOX_QNX_H_

namespace sandbox {

// Sandbox policy that allows everything (no restrictions)
class QnxSandboxPolicy {
 public:
  QnxSandboxPolicy() {}
  ~QnxSandboxPolicy() {}

  // Always returns false - sandbox is not active
  bool IsActive() const { return false; }

  // No-op - sandbox startup does nothing
  bool StartSandbox() { return true; }

  // Always returns true - all system calls are allowed
  bool AllowSyscall(int sysno) const { return true; }
};

// Check if the current process is sandboxed
inline bool IsProcessSandboxed() { return false; }

// No-op sandbox initialization
inline bool InitializeSandbox() { return true; }

// No-op pre-sandbox hook
inline void PreSandboxInit() {}

}  // namespace sandbox

// Stubs for Linux-specific sandbox defines that Chromium code references
#ifndef SECCOMP_MODE_FILTER
#define SECCOMP_MODE_FILTER 2
#endif

#ifndef PR_SET_NO_NEW_PRIVS
#define PR_SET_NO_NEW_PRIVS 38
#endif

#ifndef CLONE_NEWUSER
#define CLONE_NEWUSER 0x10000000
#endif

#ifndef CLONE_NEWPID
#define CLONE_NEWPID 0x20000000
#endif

#ifndef CLONE_NEWNET
#define CLONE_NEWNET 0x40000000
#endif

#endif  // SANDBOX_QNX_SANDBOX_QNX_H_
