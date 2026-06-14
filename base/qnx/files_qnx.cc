// Copyright 2025 SW7FT. All rights reserved.
// QNX-specific file system operations for Chromium base/

#ifdef __QNXNTO__

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <process.h>
#include <dirent.h>
#include <stdlib.h>
#include <errno.h>

namespace base {
namespace qnx {

// Get disk space info using statvfs (POSIX)
bool GetDiskSpaceInfo(const char* path, uint64_t* total_bytes,
                      uint64_t* free_bytes, uint64_t* available_bytes) {
    struct statvfs buf;
    if (::statvfs(path, &buf) != 0)
        return false;

    if (total_bytes)
        *total_bytes = (uint64_t)buf.f_blocks * buf.f_frsize;
    if (free_bytes)
        *free_bytes = (uint64_t)buf.f_bfree * buf.f_frsize;
    if (available_bytes)
        *available_bytes = (uint64_t)buf.f_bavail * buf.f_frsize;

    return true;
}

// Read /proc/<pid>/fd/<n> to get the target of a file descriptor
bool ReadFdLink(int fd, char* buf, size_t buf_size) {
    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/%d/fd/%d", (int)getpid(), fd);

    ssize_t len = ::readlink(proc_path, buf, buf_size - 1);
    if (len < 0) return false;
    buf[len] = '\0';
    return true;
}

// Get temporary directory on QNX
const char* GetTempDir() {
    const char* tmpdir = getenv("TMPDIR");
    if (tmpdir && tmpdir[0]) return tmpdir;

    struct stat st;
    if (::stat("/tmp", &st) == 0 && S_ISDIR(st.st_mode))
        return "/tmp";
    if (::stat("/var/tmp", &st) == 0 && S_ISDIR(st.st_mode))
        return "/var/tmp";

    // BB10 shared area fallback
    if (::stat("/accounts/1000/shared/misc", &st) == 0 && S_ISDIR(st.st_mode))
        return "/accounts/1000/shared/misc";

    return "/tmp";
}

// Polling-based file watcher (QNX has no inotify)
struct FileWatchState {
    char path[1024];
    struct stat last_stat;
    int active;
};

static FileWatchState g_watches[64];
static int g_watch_count = 0;

int FileWatchAdd(const char* path) {
    if (g_watch_count >= 64) return -1;

    int idx = g_watch_count++;
    int i = 0;
    while (path[i] && i < 1023) { g_watches[idx].path[i] = path[i]; i++; }
    g_watches[idx].path[i] = '\0';
    ::stat(path, &g_watches[idx].last_stat);
    g_watches[idx].active = 1;
    return idx;
}

void FileWatchRemove(int watch_id) {
    if (watch_id >= 0 && watch_id < 64)
        g_watches[watch_id].active = 0;
}

int FileWatchPoll(int watch_id) {
    if (watch_id < 0 || watch_id >= 64 || !g_watches[watch_id].active)
        return 0;

    struct stat current;
    if (::stat(g_watches[watch_id].path, &current) != 0)
        return 1;

    if (current.st_mtime != g_watches[watch_id].last_stat.st_mtime ||
        current.st_size != g_watches[watch_id].last_stat.st_size) {
        g_watches[watch_id].last_stat = current;
        return 1;
    }

    return 0;
}

}  // namespace qnx
}  // namespace base

#endif  // __QNXNTO__
