// Copyright 2025 SW7FT. All rights reserved.
// Sandbox stub implementation for QNX

#ifdef __QNXNTO__

#include <stdio.h>

namespace sandbox {

// Log that sandbox is disabled
void LogSandboxDisabled() {
    fprintf(stderr, "Chromium: Sandbox disabled on QNX (no seccomp-bpf)\n");
}

// Stub for BPF sandbox policy
class SandboxBPFPolicy {
 public:
    virtual ~SandboxBPFPolicy() {}
    virtual bool PreSandboxHook() { return true; }
};

// Stub for namespace sandbox
class NamespaceSandbox {
 public:
    static bool InNewUserNamespace() { return false; }
    static bool InNewPidNamespace() { return false; }
    static bool InNewNetNamespace() { return false; }
};

// Stub for setuid sandbox
class SetuidSandboxClient {
 public:
    static bool IsInsideSetuidSandbox() { return false; }
    static bool IsSuidSandboxChild() { return false; }
    static bool IsSuidSandboxUpToDate() { return true; }
};

// ZygoteForkDelegate stub (Chromium's multi-process model)
// On QNX, we run in single-process mode, so no zygote is needed
class ZygoteForkDelegate {
 public:
    virtual ~ZygoteForkDelegate() {}
    virtual bool CanHelp(const char* process_type) { return false; }
    virtual int Fork(const char* process_type) { return -1; }
};

}  // namespace sandbox

#endif  // __QNXNTO__
