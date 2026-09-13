// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/child/field_trial.h"

#include <tuple>

#include "base/base_switches.h"
#include "base/command_line.h"
#include "base/debug/leak_annotations.h"
#include "base/feature_list.h"
#include "base/metrics/field_trial.h"
#include "build/build_config.h"
#include "content/public/common/content_descriptors.h"
#include "content/public/common/content_switch_dependent_feature_overrides.h"
#include "content/public/common/content_switches.h"

#if BUILDFLAG(IS_QNX)
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/posix/global_descriptors.h"
#endif

namespace content {

void InitializeFieldTrialAndFeatureList() {
  const base::CommandLine& command_line =
      *base::CommandLine::ForCurrentProcess();

  // Initialize statistical testing infrastructure.
  //
  // This is intentionally leaked since it needs to live for the duration of the
  // process and there's no benefit in cleaning it up at exit.
  base::FieldTrialList* leaked_field_trial_list = new base::FieldTrialList();
  ANNOTATE_LEAKING_OBJECT_PTR(leaked_field_trial_list);
  std::ignore = leaked_field_trial_list;

  // Ensure any field trials in browser are reflected into the child process.
#if BUILDFLAG(IS_QNX)
  {
    int trial_fd =
        base::GlobalDescriptors::GetInstance()->MaybeGet(kFieldTrialDescriptor);
    int mojo_fd =
        base::GlobalDescriptors::GetInstance()->MaybeGet(kMojoIPCChannel);
    int trial_fl = trial_fd >= 0 ? fcntl(trial_fd, F_GETFL) : -1;
    int mojo_fl = mojo_fd >= 0 ? fcntl(mojo_fd, F_GETFL) : -1;
    struct stat st;
    int st_rc = (trial_fd >= 0) ? fstat(trial_fd, &st) : -1;
    fprintf(stderr,
            "QNX: child FieldTrial begin pid=%d trial_fd=%d fl=%d fstat=%d "
            "mode=%o size=%ld mojo_fd=%d mojo_fl=%d\n",
            (int)getpid(), trial_fd, trial_fl, st_rc,
            st_rc == 0 ? (int)st.st_mode : 0,
            st_rc == 0 ? (long)st.st_size : -1L, mojo_fd, mojo_fl);
    fflush(stderr);
  }
#endif
  base::FieldTrialList::CreateTrialsInChildProcess(command_line,
                                                   kFieldTrialDescriptor);
#if BUILDFLAG(IS_QNX)
  fprintf(stderr, "QNX: child FieldTrial mapped pid=%d\n", (int)getpid());
  fflush(stderr);
#endif
  std::unique_ptr<base::FeatureList> feature_list(new base::FeatureList);
  base::FieldTrialList::ApplyFeatureOverridesInChildProcess(feature_list.get());
  // TODO(crbug.com/988603): This may be redundant. The way this is supposed to
  // work is that the parent process's state should be passed via command-line
  // to the child process, such that a feature explicitly enabled or disabled in
  // the parent process via this mechanism (since the browser process also
  // registers these switch-dependent overrides), it will get passed via the
  // command line - so then no extra logic would be needed in the child.
  // TODO(chlily): Test this more thoroughly and understand the behavior to see
  // whether this is actually needed.
  feature_list->RegisterExtraFeatureOverrides(
      GetSwitchDependentFeatureOverrides(command_line));
  base::FeatureList::SetInstance(std::move(feature_list));
}

}  // namespace content
