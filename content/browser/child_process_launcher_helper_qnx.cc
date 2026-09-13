// Copyright 2026 SW7FT. All rights reserved.
// QNX child process launcher (no zygote, no sandbox).

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/memory_mapped_file.h"
#include "base/path_service.h"
#include "base/posix/global_descriptors.h"
#include "base/process/launch.h"
#include "base/trace_event/trace_event.h"
#include "content/browser/child_process_launcher.h"
#include "content/browser/child_process_launcher_helper.h"
#include "content/browser/child_process_launcher_helper_posix.h"
#include "content/common/zygote/zygote_communication_linux.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/result_codes.h"
#include "content/public/common/sandboxed_process_launcher_delegate.h"
#include "base/environment.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

extern char** environ;

namespace {

// Mirror base/process/launch_posix.cc CloseSuperfluousFds for posix_spawn.
// fork+exec closed every inherited fd except stdio and the remapped bootstrap
// fds (Mojo + field trials at kBaseDescriptor+N). posix_spawn inherits the full
// browser fd table unless we explicitly close the extras; duplicate open
// handles to the same channel/pipes break Mojo IPC and leave the renderer
// idle after startup (white screen).
//
// BB10 has no /proc/self/fd (the first implementation used DirReader on that
// path and silently closed nothing). Walk the parent's real fd table with
// fcntl(F_GETFD) instead. File actions run dup2(src→dest) first, then these
// closes, so the child's Mojo/field-trial dest fds survive and the parent's
// extra copies of those sockets do not.
void AddCloseSuperfluousFds(posix_spawn_file_actions_t* actions,
                            const base::FileHandleMappingVector& remaps) {
  long open_max = sysconf(_SC_OPEN_MAX);
  if (open_max <= 0 || open_max > 4096)
    open_max = 1024;

  int closed = 0;
  int addclose_fail = 0;
  for (int fd = 0; fd < static_cast<int>(open_max); ++fd) {
    if (fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO)
      continue;

    bool keep = false;
    for (const auto& remap : remaps) {
      if (fd == remap.second) {
        keep = true;
        break;
      }
    }
    if (keep)
      continue;

    if (fcntl(fd, F_GETFD) == -1)
      continue;

    if (posix_spawn_file_actions_addclose(actions, fd) != 0)
      ++addclose_fail;
    else
      ++closed;
  }
  fprintf(stderr,
          "QNX:LaunchChild will close %d inherited fds in child (addclose_fail=%d)\n",
          closed, addclose_fail);
}

// posix_spawn on some QNX versions silently drops extra file actions, so
// addclose alone is not enough. Mark every non-stdio parent fd CLOEXEC for
// the duration of spawn; dup2 in the child clears CLOEXEC on the remapped
// dests, then exec closes the rest.
class ScopedCloexecExceptStdio {
 public:
  ScopedCloexecExceptStdio(long open_max,
                           const base::FileHandleMappingVector& remaps) {
    if (open_max <= 0 || open_max > 4096)
      open_max = 1024;
    for (int fd = 3; fd < static_cast<int>(open_max); ++fd) {
      bool skip = false;
      for (const auto& remap : remaps) {
        if (fd == remap.first || fd == remap.second) {
          skip = true;
          break;
        }
      }
      if (skip)
        continue;
      int flags = fcntl(fd, F_GETFD);
      if (flags == -1)
        continue;
      saved_.push_back({fd, flags});
      if (!(flags & FD_CLOEXEC))
        fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
  }
  ~ScopedCloexecExceptStdio() {
    for (const auto& s : saved_)
      fcntl(s.first, F_SETFD, s.second);
  }
  int marked() const { return static_cast<int>(saved_.size()); }

 private:
  std::vector<std::pair<int, int>> saved_;
};

}  // namespace

namespace content {
namespace internal {

absl::optional<mojo::NamedPlatformChannel>
ChildProcessLauncherHelper::CreateNamedPlatformChannelOnLauncherThread() {
  DCHECK(CurrentlyOnProcessLauncherTaskRunner());
  return absl::nullopt;
}

void ChildProcessLauncherHelper::BeforeLaunchOnClientThread() {
  DCHECK(client_task_runner_->RunsTasksInCurrentSequence());
}

std::unique_ptr<FileMappedForLaunch>
ChildProcessLauncherHelper::GetFilesToMap() {
  DCHECK(CurrentlyOnProcessLauncherTaskRunner());
  return CreateDefaultPosixFilesToMap(
      child_process_id(), mojo_channel_->remote_endpoint(),
      file_data_->files_to_preload, GetProcessType(), command_line());
}

bool ChildProcessLauncherHelper::IsUsingLaunchOptions() {
  return true;
}

bool ChildProcessLauncherHelper::BeforeLaunchOnLauncherThread(
    PosixFileDescriptorInfo& files_to_register,
    base::LaunchOptions* options) {
  if (!options)
    return false;
  options->fds_to_remap = files_to_register.GetMappingWithIDAdjustment(
      base::GlobalDescriptors::kBaseDescriptor);

  base::EnvironmentMap env = delegate_->GetEnvironment();
  static const char* const kForwardEnv[] = {
      "LD_LIBRARY_PATH",
      "QNX_CA_BUNDLE",
      "TMPDIR",
      "HOME",
      "CHROME_EXE_PATH",
      "QNX_SCREEN_ROTATION",
      "QNX_TRACE",
  };
  for (const char* key : kForwardEnv) {
    const char* val = getenv(key);
    if (val && val[0])
      env[key] = val;
  }
  options->environment = env;

  fprintf(stderr, "QNX:BeforeLaunch type=%s child_id=%d fd_remap_count=%zu\n",
          GetProcessType().c_str(), child_process_id(),
          options->fds_to_remap.size());
  for (size_t i = 0; i < options->fds_to_remap.size() && i < 16; ++i) {
    fprintf(stderr, "QNX:FdRemap[%zu] src=%d dest=%d\n", i,
            options->fds_to_remap[i].first, options->fds_to_remap[i].second);
  }
  return true;
}

ChildProcessLauncherHelper::Process
ChildProcessLauncherHelper::LaunchProcessOnLauncherThread(
    const base::LaunchOptions* options,
    std::unique_ptr<FileMappedForLaunch> files_to_register,
    bool* is_synchronous_launch,
    int* launch_result) {
  *is_synchronous_launch = true;
  Process process;
  const std::string proc_type = GetProcessType();
  if (proc_type != switches::kGpuProcess) {
    command_line()->RemoveSwitch("ozone-platform");
    command_line()->RemoveSwitch("use-gl");
  }
  const auto& argv = command_line()->argv();
  fprintf(stderr, "QNX:LaunchChild type=%s pid_target=%d argc=%zu\n",
          proc_type.c_str(), child_process_id(),
          static_cast<size_t>(argv.size()));
  for (size_t i = 0; i < argv.size() && i < 24; ++i)
    fprintf(stderr, "QNX:LaunchChild argv[%zu]=%s\n", i, argv[i].c_str());

  // QNX: launch children with posix_spawn() instead of base::LaunchProcess(),
  // which uses fork()+execvp(). On QNX, fork() in a multithreaded process only
  // clones the calling thread; any allocator/runtime spinlock held by another
  // thread at fork time is never released in the child, so the child spins
  // forever (observed: child stuck single-threaded in STATE=RUNNING, still
  // showing the parent's argv) and never reaches execvp(). posix_spawn() builds
  // the new process natively from the executable image without cloning the
  // parent's locked address space, sidestepping the inherited-lock deadlock.
  std::vector<char*> argv_cstr;
  argv_cstr.reserve(argv.size() + 1);
  for (const auto& a : argv)
    argv_cstr.push_back(const_cast<char*>(a.c_str()));
  argv_cstr.push_back(nullptr);

  // Build the child environment: inherit the browser's environment, then apply
  // the overrides accumulated in BeforeLaunchOnLauncherThread (LD_LIBRARY_PATH,
  // TMPDIR, HOME, ...). An empty override value removes the key.
  std::vector<std::string> env_storage;
  const base::EnvironmentMap& overrides = options->environment;
  for (char** e = environ; e && *e; ++e) {
    const char* eq = strchr(*e, '=');
    if (!eq)
      continue;
    std::string key(*e, static_cast<size_t>(eq - *e));
    if (overrides.find(key) != overrides.end())
      continue;  // replaced (or removed) below
    env_storage.emplace_back(*e);
  }
  for (const auto& kv : overrides) {
    if (kv.second.empty())
      continue;  // removal
    env_storage.emplace_back(kv.first + "=" + kv.second);
  }
  std::vector<char*> envp;
  envp.reserve(env_storage.size() + 1);
  for (auto& s : env_storage)
    envp.push_back(const_cast<char*>(s.c_str()));
  envp.push_back(nullptr);

  // File actions: give the child /dev/null on stdin (matching base behavior),
  // then apply the fd remaps (Mojo bootstrap fd + preloaded files). dup2 clears
  // O_CLOEXEC on the destination so it survives exec; all other inherited fds
  // are O_CLOEXEC and close automatically. exec also resets caught signal
  // handlers to default, so no POSIX_SPAWN_SETSIGDEF is needed.
  auto init_remap_actions = [&](posix_spawn_file_actions_t* actions) {
    posix_spawn_file_actions_init(actions);
    posix_spawn_file_actions_addopen(actions, STDIN_FILENO, "/dev/null",
                                     O_RDONLY, 0);
    for (const auto& remap : options->fds_to_remap)
      posix_spawn_file_actions_adddup2(actions, remap.first, remap.second);
  };

  posix_spawn_file_actions_t file_actions;
  init_remap_actions(&file_actions);
  // Do not mass-addclose dozens of fds. QNX posix_spawn returns EBADF (9)
  // once the action list grows (renderer spawn failed, white screen).
  // CLOEXEC on leftover parent fds covers inheritance across exec. Restore
  // those flags before any remaps-only retry so spawn can still see remap
  // sources if QNX closes CLOEXEC fds before applying file actions.
  pid_t child_pid = 0;
  const char* exe_path = argv_cstr[0];
  int spawn_rc = 0;
  {
    ScopedCloexecExceptStdio cloexec(sysconf(_SC_OPEN_MAX),
                                     options->fds_to_remap);
    fprintf(stderr, "QNX:LaunchChild marked %d parent fds CLOEXEC for spawn\n",
            cloexec.marked());
    spawn_rc = posix_spawn(&child_pid, exe_path, &file_actions,
                           /*attrp=*/nullptr, argv_cstr.data(), envp.data());
  }
  posix_spawn_file_actions_destroy(&file_actions);
  if (spawn_rc == EBADF) {
    fprintf(stderr,
            "QNX:LaunchChild type=%s spawn EBADF, retry remaps-only\n",
            proc_type.c_str());
    posix_spawn_file_actions_t retry_actions;
    init_remap_actions(&retry_actions);
    child_pid = 0;
    spawn_rc = posix_spawn(&child_pid, exe_path, &retry_actions,
                           /*attrp=*/nullptr, argv_cstr.data(), envp.data());
    posix_spawn_file_actions_destroy(&retry_actions);
  }

  if (spawn_rc == 0 && child_pid > 0) {
    process.process = base::Process(child_pid);
    *launch_result = LAUNCH_RESULT_SUCCESS;
    fprintf(stderr, "QNX:LaunchChild type=%s spawned pid=%d (posix_spawn)\n",
            proc_type.c_str(), static_cast<int>(child_pid));
  } else {
    process.process = base::Process();
    *launch_result = LAUNCH_RESULT_FAILURE;
    fprintf(stderr,
            "QNX:LaunchChild type=%s posix_spawn FAILED rc=%d errno=%d\n",
            proc_type.c_str(), spawn_rc, errno);
  }
  return process;
}

void ChildProcessLauncherHelper::AfterLaunchOnLauncherThread(
    const ChildProcessLauncherHelper::Process& process,
    const base::LaunchOptions* options) {
  file_data_.reset();
}

ChildProcessTerminationInfo ChildProcessLauncherHelper::GetTerminationInfo(
    const ChildProcessLauncherHelper::Process& process,
    bool known_dead) {
  ChildProcessTerminationInfo info;
  if (known_dead) {
    info.status = base::GetKnownDeadTerminationStatus(process.process.Handle(),
                                                        &info.exit_code);
  } else {
    info.status =
        base::GetTerminationStatus(process.process.Handle(), &info.exit_code);
  }
  return info;
}

bool ChildProcessLauncherHelper::TerminateProcess(const base::Process& process,
                                                  int exit_code) {
  return process.Terminate(exit_code, false);
}

void ChildProcessLauncherHelper::ForceNormalProcessTerminationSync(
    ChildProcessLauncherHelper::Process process) {
  TRACE_EVENT0("content",
               "ChildProcessLauncherHelper::ForceNormalProcessTerminationSync");
  DCHECK(CurrentlyOnProcessLauncherTaskRunner());
  process.process.Terminate(RESULT_CODE_NORMAL_EXIT, false);
  base::EnsureProcessTerminated(std::move(process.process));
}

void ChildProcessLauncherHelper::SetProcessPriorityOnLauncherThread(
    base::Process process,
    base::Process::Priority priority) {
  DCHECK(CurrentlyOnProcessLauncherTaskRunner());
  (void)process;
  (void)priority;
}

ZygoteCommunication* ChildProcessLauncherHelper::GetZygoteForLaunch() {
  return nullptr;
}

base::File OpenFileToShare(const base::FilePath& path,
                           base::MemoryMappedFile::Region* region) {
  base::FilePath exe_dir;
  bool result = base::PathService::Get(base::BasePathKey::DIR_EXE, &exe_dir);
  DCHECK(result);
  base::File file(exe_dir.Append(path),
                  base::File::FLAG_OPEN | base::File::FLAG_READ);
  *region = base::MemoryMappedFile::Region::kWholeFile;
  return file;
}

}  // namespace internal
}  // namespace content
