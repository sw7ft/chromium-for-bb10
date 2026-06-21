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
#include "base/files/dir_reader_posix.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <string>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace {

constexpr char kFDDir[] = "/proc/self/fd";

// Mirror base/process/launch_posix.cc CloseSuperfluousFds for posix_spawn.
// fork+exec closed every inherited fd except stdio and the remapped bootstrap
// fds (Mojo + field trials at kBaseDescriptor+N). posix_spawn inherits the full
// browser fd table unless we explicitly close the extras; duplicate open
// handles to the same channel/pipes can break Mojo IPC and leave the renderer
// idle after startup (white screen, "renderer unresponsive").
void AddCloseSuperfluousFds(posix_spawn_file_actions_t* actions,
                            const base::FileHandleMappingVector& remaps) {
  base::DirReaderPosix fd_dir(kFDDir);
  if (!fd_dir.IsValid())
    return;

  const int dir_fd = fd_dir.fd();
  for (; fd_dir.Next(); ) {
    if (fd_dir.name()[0] == '.')
      continue;

    char* endptr = nullptr;
    errno = 0;
    const long fd_l = strtol(fd_dir.name(), &endptr, 10);
    if (fd_dir.name()[0] == 0 || *endptr || fd_l < 0 || errno)
      continue;

    const int fd = static_cast<int>(fd_l);
    if (fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO)
      continue;
    if (fd == dir_fd)
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

    // Ignore errors: if a fd isn't inherited, addclose is a no-op on some
    // platforms; on others we skip silently via spawn failure logging below.
    posix_spawn_file_actions_addclose(actions, fd);
  }
}

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
  posix_spawn_file_actions_t file_actions;
  posix_spawn_file_actions_init(&file_actions);
  posix_spawn_file_actions_addopen(&file_actions, STDIN_FILENO, "/dev/null",
                                   O_RDONLY, 0);
  for (const auto& remap : options->fds_to_remap) {
    posix_spawn_file_actions_adddup2(&file_actions, remap.first, remap.second);
  }
  AddCloseSuperfluousFds(&file_actions, options->fds_to_remap);

  pid_t child_pid = 0;
  const char* exe_path = argv_cstr[0];
  int spawn_rc = posix_spawn(&child_pid, exe_path, &file_actions,
                             /*attrp=*/nullptr, argv_cstr.data(), envp.data());
  posix_spawn_file_actions_destroy(&file_actions);

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
