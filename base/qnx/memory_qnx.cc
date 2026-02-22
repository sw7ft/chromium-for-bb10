// Copyright 2025 SW7FT. All rights reserved.
// QNX-specific memory management for Chromium base/

#ifdef __QNXNTO__

#include <sys/types.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <process.h>
#include <stdlib.h>
#include <errno.h>

namespace base {
namespace qnx {

// POSIX shared memory (QNX doesn't have memfd_create)
int CreateAnonymousSharedMemory(size_t size) {
    static int counter = 0;
    char name[64];
    snprintf(name, sizeof(name), "/chromium_shm_%d_%d", (int)getpid(), counter++);

    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;

    // Immediately unlink for anonymity
    shm_unlink(name);

    if (ftruncate(fd, size) != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

// Map executable memory for V8 JIT
void* MapExecutableMemory(size_t size) {
    void* addr = mmap(NULL, size,
                      PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANON,
                      -1, 0);
    if (addr == MAP_FAILED) return NULL;
    return addr;
}

// Make memory executable (for JIT)
bool MakeMemoryExecutable(void* addr, size_t size) {
    return mprotect(addr, size, PROT_READ | PROT_EXEC) == 0;
}

// Make memory writable (for JIT patching)
bool MakeMemoryWritable(void* addr, size_t size) {
    return mprotect(addr, size, PROT_READ | PROT_WRITE) == 0;
}

// Discard memory pages
bool DiscardMemory(void* addr, size_t size) {
    // QNX supports POSIX_MADV_DONTNEED
    return posix_madvise(addr, size, POSIX_MADV_DONTNEED) == 0;
}

// Get system memory info
bool GetSystemMemoryInfo(size_t* total_bytes, size_t* free_bytes) {
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGESIZE);

    if (pages < 0 || page_size < 0) return false;

    if (total_bytes) *total_bytes = (size_t)pages * page_size;

    long avail_pages = sysconf(_SC_AVPHYS_PAGES);
    if (free_bytes) {
        if (avail_pages >= 0)
            *free_bytes = (size_t)avail_pages * page_size;
        else
            *free_bytes = 0;
    }

    return true;
}

}  // namespace qnx
}  // namespace base

#endif  // __QNXNTO__
