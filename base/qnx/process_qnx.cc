// Copyright 2025 SW7FT. All rights reserved.
// QNX-specific process management for Chromium base/

#ifdef __QNXNTO__

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/procfs.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>
#include <process.h>
#include <stdlib.h>
#include <errno.h>
#include <devctl.h>

extern char **environ;

namespace base {
namespace qnx {

// Get the executable path of the current process
bool GetCurrentProcessExePath(char* path, size_t path_size) {
    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/%d/exefile", (int)getpid());

    int fd = open(proc_path, O_RDONLY);
    if (fd < 0) return false;

    ssize_t len = read(fd, path, path_size - 1);
    close(fd);

    if (len <= 0) return false;
    path[len] = '\0';

    if (len > 0 && path[len - 1] == '\n')
        path[len - 1] = '\0';

    return true;
}

// Get memory usage for a process
bool GetProcessMemoryInfo(pid_t pid, size_t* rss_bytes, size_t* vss_bytes) {
    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/%d/as", (int)pid);

    int fd = open(proc_path, O_RDONLY);
    if (fd < 0) return false;

    int num_maps = 0;
    if (devctl(fd, DCMD_PROC_MAPINFO, NULL, 0, &num_maps) != EOK) {
        close(fd);
        return false;
    }

    procfs_mapinfo* maps = (procfs_mapinfo*)malloc(num_maps * sizeof(procfs_mapinfo));
    if (!maps) {
        close(fd);
        return false;
    }

    if (devctl(fd, DCMD_PROC_PAGEDATA, maps, num_maps * sizeof(procfs_mapinfo), &num_maps) != EOK) {
        free(maps);
        close(fd);
        return false;
    }

    size_t rss = 0, vss = 0;
    for (int i = 0; i < num_maps; i++) {
        vss += maps[i].size;
        if (maps[i].flags & MAP_PRIVATE) {
            rss += maps[i].size;
        }
    }

    if (rss_bytes) *rss_bytes = rss;
    if (vss_bytes) *vss_bytes = vss;

    free(maps);
    close(fd);
    return true;
}

// Launch a child process using posix_spawn (preferred on QNX)
pid_t SpawnProcess(const char* path, char* const argv[], char* const envp[],
                   int stdin_fd, int stdout_fd, int stderr_fd) {
    pid_t child_pid;
    posix_spawn_file_actions_t file_actions;
    posix_spawnattr_t attr;

    posix_spawn_file_actions_init(&file_actions);
    posix_spawnattr_init(&attr);

    if (stdin_fd >= 0) {
        posix_spawn_file_actions_adddup2(&file_actions, stdin_fd, STDIN_FILENO);
    }
    if (stdout_fd >= 0) {
        posix_spawn_file_actions_adddup2(&file_actions, stdout_fd, STDOUT_FILENO);
    }
    if (stderr_fd >= 0) {
        posix_spawn_file_actions_adddup2(&file_actions, stderr_fd, STDERR_FILENO);
    }

    int result = posix_spawn(&child_pid, path, &file_actions, &attr,
                             argv, envp ? envp : environ);

    posix_spawn_file_actions_destroy(&file_actions);
    posix_spawnattr_destroy(&attr);

    if (result != 0) {
        errno = result;
        return -1;
    }

    return child_pid;
}

}  // namespace qnx
}  // namespace base

#endif  // __QNXNTO__
