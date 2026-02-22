// Copyright 2025 SW7FT. All rights reserved.
// QNX platform detection and compatibility header for Chromium base/
//
// QNX is POSIX-compliant but differs from Linux in several key areas:
//   - No /proc/self/ (uses /proc/<pid>/ instead)
//   - No epoll (uses select/poll or ionotify)
//   - No prctl() (no process control)
//   - No clone() (use posix_spawn or fork+exec)
//   - No seccomp-bpf (no sandbox)
//   - Different shared memory API (shm_open works)
//   - No eventfd (use pipes or pulse channels)
//   - No signalfd (use sigwaitinfo)
//   - No inotify (use QNX ionotify or polling)
//
// This header provides compatibility macros and function declarations.

#ifndef BASE_QNX_QNX_PLATFORM_H_
#define BASE_QNX_QNX_PLATFORM_H_

#ifdef __QNXNTO__

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/procfs.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <stdlib.h>

// ---- /proc/self/ compatibility ----
// QNX uses /proc/<pid>/ instead of /proc/self/
// Provide a helper that returns the correct path

static inline int qnx_proc_self_fd(const char* suffix, int flags) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/%s", getpid(), suffix);
    return open(path, flags);
}

// ---- prctl() stub ----
// QNX doesn't have prctl(). Stub the common operations.
#ifndef PR_SET_NAME
#define PR_SET_NAME 15
#endif
#ifndef PR_GET_NAME
#define PR_GET_NAME 16
#endif
#ifndef PR_SET_DUMPABLE
#define PR_SET_DUMPABLE 4
#endif
#ifndef PR_SET_PDEATHSIG
#define PR_SET_PDEATHSIG 1
#endif

static inline int qnx_prctl(int option, unsigned long arg2,
                             unsigned long arg3, unsigned long arg4,
                             unsigned long arg5) {
    switch (option) {
        case PR_SET_NAME: {
            // Set thread name via pthread
            return pthread_setname_np(pthread_self(), (const char*)arg2);
        }
        case PR_GET_NAME: {
            // QNX doesn't have a direct equivalent
            strncpy((char*)arg2, "thread", 16);
            return 0;
        }
        case PR_SET_DUMPABLE:
        case PR_SET_PDEATHSIG:
            // No-op on QNX
            return 0;
        default:
            errno = EINVAL;
            return -1;
    }
}

// ---- eventfd emulation ----
// QNX lacks eventfd. We emulate it with a socketpair(AF_UNIX, SOCK_DGRAM).
// A DGRAM socketpair allows both read and write on each end independently,
// matching eventfd's single-fd semantics. We return fds[0] and close fds[1]
// after connecting them, so the returned fd is self-loopback: writes appear
// as readable data on the same fd.

#include <sys/socket.h>

#ifndef EFD_CLOEXEC
#define EFD_CLOEXEC  02000000
#endif
#ifndef EFD_NONBLOCK
#define EFD_NONBLOCK 04000
#endif
#ifndef EFD_SEMAPHORE
#define EFD_SEMAPHORE 1
#endif

typedef uint64_t eventfd_t;

// Global table mapping read-end fd -> write-end fd for the pipe-based fallback.
// Max 64 simultaneous eventfds should be more than enough.
#define QNX_EFD_TABLE_SIZE 256
static int qnx_efd_write_end[QNX_EFD_TABLE_SIZE];
static int qnx_efd_table_init = 0;

static inline void qnx_efd_init_table(void) {
    if (!qnx_efd_table_init) {
        for (int i = 0; i < QNX_EFD_TABLE_SIZE; i++)
            qnx_efd_write_end[i] = -1;
        qnx_efd_table_init = 1;
    }
}

static inline int qnx_eventfd(unsigned int initval, int flags) {
    qnx_efd_init_table();

    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;

    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    fcntl(pipefd[1], F_SETFL, O_NONBLOCK);
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);

    if (pipefd[0] < QNX_EFD_TABLE_SIZE)
        qnx_efd_write_end[pipefd[0]] = pipefd[1];

    if (initval > 0) {
        uint64_t val = initval;
        write(pipefd[1], &val, sizeof(val));
    }

    return pipefd[0];
}

static inline int eventfd_read(int fd, eventfd_t* value) {
    ssize_t r = read(fd, value, sizeof(*value));
    return (r == (ssize_t)sizeof(*value)) ? 0 : -1;
}

static inline int eventfd_write(int fd, eventfd_t value) {
    qnx_efd_init_table();
    int wfd = (fd >= 0 && fd < QNX_EFD_TABLE_SIZE) ? qnx_efd_write_end[fd] : -1;
    if (wfd < 0) wfd = fd;
    ssize_t r = write(wfd, &value, sizeof(value));
    return (r == (ssize_t)sizeof(value)) ? 0 : -1;
}

#define eventfd(initval, flags) qnx_eventfd(initval, flags)

// ---- signalfd stub ----
// QNX doesn't have signalfd. Use sigwaitinfo instead.
// For Chromium's usage, we stub it out since sandboxing is disabled.
#ifndef SFD_CLOEXEC
#define SFD_CLOEXEC 0
#endif
#ifndef SFD_NONBLOCK
#define SFD_NONBLOCK 0
#endif

// ---- inotify stub ----
// QNX doesn't have inotify. Chromium uses it for file watching.
// Stub it out - file watching will use polling fallback.
#ifndef IN_MODIFY
#define IN_MODIFY     0x00000002
#define IN_ATTRIB     0x00000004
#define IN_CLOSE_WRITE 0x00000008
#define IN_MOVED_FROM 0x00000040
#define IN_MOVED_TO   0x00000080
#define IN_CREATE     0x00000100
#define IN_DELETE     0x00000200
#define IN_DELETE_SELF 0x00000400
#define IN_MOVE_SELF  0x00000800
#endif

static inline int qnx_inotify_init1(int flags) {
    errno = ENOSYS;
    return -1;
}

static inline int qnx_inotify_add_watch(int fd, const char* path, uint32_t mask) {
    errno = ENOSYS;
    return -1;
}

static inline int qnx_inotify_rm_watch(int fd, int wd) {
    errno = ENOSYS;
    return -1;
}

// ---- sysconf extensions ----
// QNX's sysconf supports _SC_NPROCESSORS_ONLN
// Already POSIX-compliant, just noting for documentation

// ---- Process spawning ----
// QNX prefers posix_spawn over fork+exec (fork is expensive on QNX)
// Chromium's process launching should use posix_spawn when on QNX

// ---- Memory mapping ----
// QNX supports mmap, mprotect, munmap (standard POSIX)
// MAP_ANONYMOUS is available
// shm_open/shm_unlink work for shared memory

#endif  // __QNXNTO__
#endif  // BASE_QNX_QNX_PLATFORM_H_
