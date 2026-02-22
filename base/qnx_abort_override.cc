#include "build/build_config.h"

#if BUILDFLAG(IS_QNX)
#include <dlfcn.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void qnx_print_caller(const char* prefix, void* addr) {
  char buf[512];
  Dl_info dli;
  if (addr) {
    if (dladdr(addr, &dli) && dli.dli_sname) {
      int n = snprintf(buf, sizeof(buf), "%s=%p %s:%s+0x%x\n", prefix, addr,
                       dli.dli_fname ? dli.dli_fname : "?", dli.dli_sname,
                       (unsigned)((char*)addr - (char*)dli.dli_saddr));
      write(2, buf, n);
    } else {
      int n = snprintf(buf, sizeof(buf), "%s=%p (no sym)\n", prefix, addr);
      write(2, buf, n);
    }
  }
}

static void qnx_dump_stack(const char* tag) {
  char buf[512];
  Dl_info dli;
  void* sp;
  __asm__ volatile("mov %0, sp" : "=r"(sp));
  uintptr_t* sp_ptr = (uintptr_t*)sp;
  int found = 0;
  for (int i = 0; i < 64 && found < 20; i++) {
    uintptr_t word = sp_ptr[i];
    if (word > 0x800000 && word < 0x8000000) {
      if (dladdr((void*)word, &dli) && dli.dli_sname) {
        int n = snprintf(buf, sizeof(buf), "QNX:%s[%d]=0x%lx %s+0x%x\n", tag,
                         i, (unsigned long)word, dli.dli_sname,
                         (unsigned)((char*)(uintptr_t)word -
                                    (char*)dli.dli_saddr));
        write(2, buf, n);
      } else {
        int n = snprintf(buf, sizeof(buf), "QNX:%s[%d]=0x%lx\n", tag, i,
                         (unsigned long)word);
        write(2, buf, n);
      }
      found++;
    }
  }
}

// Override std::__throw_bad_optional_access to capture the .value() call site.
// Since std::optional::value() is inlined, __builtin_return_address(0) gives
// the address of the user code that called .value().
namespace std {
__attribute__((visibility("default"), noreturn)) void
__throw_bad_optional_access() {
  write(2, "QNX:BAD_OPTIONAL\n", 17);
  qnx_print_caller("QNX:OPT_C0", __builtin_return_address(0));
  qnx_dump_stack("OS");
  abort();
}
}  // namespace std

// Use --wrap=abort linker flag so __wrap_abort intercepts all calls
extern "C" void __real_abort(void) __attribute__((noreturn));
extern "C" void __wrap_abort(void) __attribute__((noreturn));

extern "C" void __wrap_abort(void) {
  write(2, "QNX:ABORT_WRAP\n", 15);
  qnx_print_caller("QNX:ABORT_C0", __builtin_return_address(0));
  qnx_dump_stack("S");
  __real_abort();
}
#endif
