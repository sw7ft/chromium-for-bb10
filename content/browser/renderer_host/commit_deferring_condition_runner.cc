// Copyright 2021 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/renderer_host/commit_deferring_condition_runner.h"

#if BUILDFLAG(IS_QNX)
#include <cstdio>
#include <unistd.h>
#endif
#include "base/memory/ptr_util.h"
#include "base/no_destructor.h"
#include "content/browser/renderer_host/back_forward_cache_commit_deferring_condition.h"
#include "content/browser/renderer_host/concurrent_navigations_commit_deferring_condition.h"
#include "content/browser/renderer_host/navigation_request.h"
#include "content/browser/renderer_host/navigator_delegate.h"
#include "content/browser/renderer_host/view_transition_commit_deferring_condition.h"
#include "content/common/content_navigation_policy.h"
#include "content/common/features.h"
#include "content/public/browser/commit_deferring_condition.h"

namespace content {

namespace {

using GeneratorOrderPair =
    std::pair<CommitDeferringConditionRunner::ConditionGenerator,
              CommitDeferringConditionRunner::InsertOrder>;

std::map<int, GeneratorOrderPair>& GetConditionGenerators() {
  static base::NoDestructor<std::map<int, GeneratorOrderPair>> generators;
  return *generators;
}

}  // namespace

// static
std::unique_ptr<CommitDeferringConditionRunner>
CommitDeferringConditionRunner::Create(
    NavigationRequest& navigation_request,
    CommitDeferringCondition::NavigationType navigation_type,
    absl::optional<int> candidate_prerender_frame_tree_node_id) {
  auto runner = base::WrapUnique(new CommitDeferringConditionRunner(
      navigation_request, navigation_type,
      candidate_prerender_frame_tree_node_id));
  return runner;
}

CommitDeferringConditionRunner::CommitDeferringConditionRunner(
    Delegate& delegate,
    CommitDeferringCondition::NavigationType navigation_type,
    absl::optional<int> candidate_prerender_frame_tree_node_id)
    : delegate_(delegate),
      navigation_type_(navigation_type),
      candidate_prerender_frame_tree_node_id_(
          candidate_prerender_frame_tree_node_id) {}

CommitDeferringConditionRunner::~CommitDeferringConditionRunner() = default;

void CommitDeferringConditionRunner::ProcessChecks() {
  ProcessConditions();
}

void CommitDeferringConditionRunner::AddConditionForTesting(
    std::unique_ptr<CommitDeferringCondition> condition) {
  AddCondition(std::move(condition));
}

CommitDeferringCondition*
CommitDeferringConditionRunner::GetDeferringConditionForTesting() const {
  if (!is_deferred_)
    return nullptr;

  DCHECK(!conditions_.empty());
  return (*conditions_.begin()).get();
}

void CommitDeferringConditionRunner::ResumeProcessing() {
  DCHECK(is_deferred_);
  is_deferred_ = false;

  // This is resuming from a check that resolved asynchronously. The current
  // check is always at the front of the vector so pop it and then proceed with
  // the next one.
  DCHECK(!conditions_.empty());
  conditions_.erase(conditions_.begin());
  ProcessConditions();
}

void CommitDeferringConditionRunner::RegisterDeferringConditions(
    NavigationRequest& navigation_request) {
  switch (navigation_type_) {
    case CommitDeferringCondition::NavigationType::kPrerenderedPageActivation:
      // For prerendered page activation, conditions should run before start
      // navigation.
      DCHECK_LT(navigation_request.state(),
                NavigationRequest::WILL_START_NAVIGATION);
      break;
    case CommitDeferringCondition::NavigationType::kOther:
      // For other navigations, conditions should run before navigation commit.
      DCHECK_EQ(navigation_request.state(),
                NavigationRequest::WILL_PROCESS_RESPONSE);
      break;
  }

  // Let WebContents add deferring conditions.
  std::vector<std::unique_ptr<CommitDeferringCondition>> delegate_conditions =
      navigation_request.GetDelegate()
          ->CreateDeferringConditionsForNavigationCommit(navigation_request,
                                                         navigation_type_);
  for (auto& condition : delegate_conditions) {
    DCHECK(condition);
    AddCondition(std::move(condition));
  }

  AddCondition(PrerenderCommitDeferringCondition::MaybeCreate(
      navigation_request, navigation_type_,
      candidate_prerender_frame_tree_node_id_));

  AddCondition(
      ViewTransitionCommitDeferringCondition::MaybeCreate(navigation_request));

  if (ShouldAvoidRedundantNavigationCancellations()) {
    AddCondition(ConcurrentNavigationsCommitDeferringCondition::MaybeCreate(
        navigation_request, navigation_type_));
  }

  // The BFCache deferring condition should run after all other conditions
  // since it'll disable eviction on a cached renderer.
  AddCondition(BackForwardCacheCommitDeferringCondition::MaybeCreate(
      navigation_request));

  // Run condition generators for testing.
  for (auto& iter : GetConditionGenerators()) {
    GeneratorOrderPair& generator_order_pair = iter.second;
    AddCondition(
        generator_order_pair.first.Run(navigation_request, navigation_type_),
        generator_order_pair.second);
  }
}

// static
int CommitDeferringConditionRunner::InstallConditionGeneratorForTesting(
    ConditionGenerator generator,
    InsertOrder order) {
  static int generator_id = 0;
  GetConditionGenerators().emplace(generator_id,
                                   std::make_pair(std::move(generator), order));
  return generator_id++;
}

// static
void CommitDeferringConditionRunner::UninstallConditionGeneratorForTesting(
    int generator_id) {
  GetConditionGenerators().erase(generator_id);
}

void CommitDeferringConditionRunner::ProcessConditions() {
#if BUILDFLAG(IS_QNX)
  {
    char _b[64];
    int _n = snprintf(_b, sizeof(_b), "QNX:CDC:0 count=%zu\n", conditions_.size());
    write(2, _b, _n);
  }
#endif
  while (!conditions_.empty()) {
    auto resume_closure =
        base::BindOnce(&CommitDeferringConditionRunner::ResumeProcessing,
                       weak_factory_.GetWeakPtr());
    CommitDeferringCondition* condition = (*conditions_.begin()).get();
#if BUILDFLAG(IS_QNX)
    {
      char _b[128];
      int _n = snprintf(_b, sizeof(_b), "QNX:CDC:chk %p\n",
                        (void*)condition);
      write(2, _b, _n);
    }
#endif
    is_deferred_ = false;
    auto result = condition->WillCommitNavigation(std::move(resume_closure));
#if BUILDFLAG(IS_QNX)
    {
      char _b[128];
      int _n = snprintf(_b, sizeof(_b), "QNX:CDC:res %d\n",
                        (int)result);
      write(2, _b, _n);
    }
#endif
    switch (result) {
      case CommitDeferringCondition::Result::kDefer:
        is_deferred_ = true;
        return;
      case CommitDeferringCondition::Result::kCancelled:
        return;
      case CommitDeferringCondition::Result::kProceed:
        break;
    }

    conditions_.erase(conditions_.begin());
  }

#if BUILDFLAG(IS_QNX)
  write(2, "QNX:CDC:done allOK\n", 19);
#endif
  delegate_->OnCommitDeferringConditionChecksComplete(
      navigation_type_, candidate_prerender_frame_tree_node_id_);
}

void CommitDeferringConditionRunner::AddCondition(
    std::unique_ptr<CommitDeferringCondition> condition,
    InsertOrder order) {
  if (!condition)
    return;

  if (order == InsertOrder::kAfter)
    conditions_.push_back(std::move(condition));
  else
    conditions_.insert(conditions_.begin(), std::move(condition));
}

}  // namespace content
