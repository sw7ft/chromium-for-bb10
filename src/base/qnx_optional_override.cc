#include "build/build_config.h"

#if BUILDFLAG(IS_QNX)
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

namespace std {

[[noreturn]] void __throw_bad_optional_access() {
  char buf[512];
  write(2, "QNX:BAD_OPTIONAL\n", 17);

  // __builtin_return_address(0) gives the call site of
  // __throw_bad_optional_access, which — when value() is inlined — is
  // the user code that called .value().
  void* ret0 = __builtin_return_address(0);
  Dl_info dli;
  if (ret0) {
    if (dladdr(ret0, &dli) && dli.dli_sname) {
      int n = snprintf(buf, sizeof(buf), "QNX:OPT_C0=%p %s:%s+0x%x\n", ret0,
                        dli.dli_fname ? dli.dli_fname : "?", dli.dli_sname,
                        (unsigned)((char*)ret0 - (char*)dli.dli_saddr));
      write(2, buf, n);
    } else {
      int n = snprintf(buf, sizeof(buf), "QNX:OPT_C0=%p (no sym)\n", ret0);
      write(2, buf, n);
    }
  }

  // Try one more frame — may fail without frame pointers but worth a shot.
  void* ret1 = __builtin_return_address(1);
  if (ret1) {
    if (dladdr(ret1, &dli) && dli.dli_sname) {
      int n = snprintf(buf, sizeof(buf), "QNX:OPT_C1=%p %s:%s+0x%x\n", ret1,
                        dli.dli_fname ? dli.dli_fname : "?", dli.dli_sname,
                        (unsigned)((char*)ret1 - (char*)dli.dli_saddr));
      write(2, buf, n);
    } else {
      int n = snprintf(buf, sizeof(buf), "QNX:OPT_C1=%p (no sym)\n", ret1);
      write(2, buf, n);
    }
  }

  // Raw stack scan — binary is ~95 MB so text extends to ~0x5A00000.
  // Also check shared-lib range (0x6000000+).
  void* sp;
  __asm__ volatile("mov %0, sp" : "=r"(sp));
  uintptr_t* sp_ptr = (uintptr_t*)sp;
  int found = 0;
  for (int i = 0; i < 64 && found < 20; i++) {
    uintptr_t word = sp_ptr[i];
    if (word > 0x800000 && word < 0x8000000) {
      if (dladdr((void*)word, &dli) && dli.dli_sname) {
        int n =
            snprintf(buf, sizeof(buf), "QNX:OS[%d]=0x%lx %s+0x%x\n", i,
                     (unsigned long)word, dli.dli_sname,
                     (unsigned)((char*)(uintptr_t)word - (char*)dli.dli_saddr));
        write(2, buf, n);
      } else {
        int n = snprintf(buf, sizeof(buf), "QNX:OS[%d]=0x%lx\n", i,
                         (unsigned long)word);
        write(2, buf, n);
      }
      found++;
    }
  }

  abort();
}

}  // namespace std

#endif
