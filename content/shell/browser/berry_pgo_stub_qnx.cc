// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Production QNX builds call __llvm_profile_write_file() only when
// BERRY_PGO_COLLECT=1 (restart/exit flush). The profile runtime is linked only
// in chrome_pgo_phase=1 instrumented builds. Provide a weak no-op so normal
// links succeed; instrumented builds override with libclang_rt.profile.
extern "C" __attribute__((weak)) int __llvm_profile_write_file(void) {
  return 0;
}
