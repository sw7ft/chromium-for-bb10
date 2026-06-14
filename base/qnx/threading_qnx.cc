// Copyright 2025 SW7FT. All rights reserved.
// QNX-specific threading support for Chromium base/

#ifdef __QNXNTO__

#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <errno.h>

namespace base {
namespace qnx {

// Set thread name
int SetCurrentThreadName(const char* name) {
    char truncated[100];
    int i = 0;
    while (name[i] && i < 99) { truncated[i] = name[i]; i++; }
    truncated[i] = '\0';
    // pthread_setname_np is a GNU extension available on QNX
    // Use the POSIX fallback if not available
    return 0;  // No-op stub; QNX may not expose pthread_setname_np in all versions
}

// Set thread priority using QNX scheduling
int SetThreadPriority(pthread_t thread, int chromium_priority) {
    struct sched_param param;
    int policy;

    pthread_getschedparam(thread, &policy, &param);

    switch (chromium_priority) {
        case 0:  param.sched_priority = 5;  policy = SCHED_OTHER; break;
        case 1:  param.sched_priority = 10; policy = SCHED_OTHER; break;
        case 2:  param.sched_priority = 20; policy = SCHED_OTHER; break;
        case 3:  param.sched_priority = 30; policy = SCHED_RR;    break;
        default: param.sched_priority = 10; policy = SCHED_OTHER; break;
    }

    return pthread_setschedparam(thread, policy, &param);
}

// Get number of CPUs
int GetNumberOfProcessors() {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (int)n : 1;
}

}  // namespace qnx
}  // namespace base

#endif  // __QNXNTO__
