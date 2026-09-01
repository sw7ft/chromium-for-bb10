// QNX compatibility for LLVM compiler-rt profile runtime.
#ifndef BERRY_QNX_PROFILE_COMPAT_H_
#define BERRY_QNX_PROFILE_COMPAT_H_

#include <stddef.h>
#include <sys/mman.h>

// QNX provides posix_madvise but not the BSD madvise() name.
static inline int madvise(void* addr, size_t len, int advice) {
  return posix_madvise(addr, len, advice);
}

#endif  // BERRY_QNX_PROFILE_COMPAT_H_
