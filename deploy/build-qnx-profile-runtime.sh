#!/usr/bin/env bash
# Cross-build libclang_rt.profile for QNX ARM (LLVM 17 instrumentation PGO).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LLVM_SRC="${LLVM_SRC:-/root/llvm-project-17}"
OUT_DIR="${OUT_DIR:-/root/llvm-qnx-profile}"
PROFILE_DIR="${LLVM_SRC}/compiler-rt/lib/profile"
QNX_SYS="/root/qnx800/arm-blackberry-qnx8eabi"
QNX_INC="/root/qnx800/include"
TRIPLE="armv7-unknown-nto-qnx8.0.0eabi"

COMMON_C=(
  "--target=${TRIPLE}"
  -march=armv7-a -mfloat-abi=softfp -mfpu=neon-vfpv3 -mthumb -fPIC
  -D__QNX__ -D__QNXNTO__ -D__ARM__ -D__LITTLEENDIAN__ -D_QNX_SOURCE
  -DMADV_DONTNEED=4 -DMADV_FREE=8
  -DCOMPILER_RT_HAS_ATOMICS=1
  -DCOMPILER_RT_HAS_FCNTL_LCK=1
  -DCOMPILER_RT_HAS_UNAME=1
  -nostdinc
  -isystem "${QNX_INC}"
  -isystem /usr/lib/llvm-17/lib/clang/17/include
  -I"${PROFILE_DIR}"
  -I"${LLVM_SRC}/compiler-rt/lib"
  -I"${LLVM_SRC}/compiler-rt/include"
  -include "${SCRIPT_DIR}/qnx_profile_compat.h"
  -O2 -Wno-unused-function
)

COMMON_CXX=(
  "${COMMON_C[@]}"
  -nostdinc++
  -isystem "${QNX_SYS}/usr/include/libstdc++/9.3.0"
  -isystem "${QNX_SYS}/usr/include/libstdc++/9.3.0/arm-blackberry-qnx8eabi"
  -fno-exceptions -fno-rtti
)

SOURCES=(
  GCDAProfiling.c
  InstrProfiling.c
  InstrProfilingBuffer.c
  InstrProfilingFile.c
  InstrProfilingInternal.c
  InstrProfilingMerge.c
  InstrProfilingMergeFile.c
  InstrProfilingNameVar.c
  InstrProfilingPlatformOther.c
  InstrProfilingUtil.c
  InstrProfilingValue.c
  InstrProfilingVersionVar.c
  InstrProfilingWriter.c
)

mkdir -p "${OUT_DIR}/obj"
rm -f "${OUT_DIR}/obj"/*

for src in "${SOURCES[@]}"; do
  base="${src%.c}"
  echo "CC ${src}"
  clang-17 -c "${PROFILE_DIR}/${src}" -o "${OUT_DIR}/obj/${base}.o" "${COMMON_C[@]}"
done

echo "CXX InstrProfilingRuntime.cpp"
clang++-17 -c "${PROFILE_DIR}/InstrProfilingRuntime.cpp" \
  -o "${OUT_DIR}/obj/InstrProfilingRuntime.o" "${COMMON_CXX[@]}"

AR="/root/qnx800/bin/arm-blackberry-qnx8eabi-ar"
"${AR}" rcs "${OUT_DIR}/libclang_rt.profile-arm.a" "${OUT_DIR}/obj"/*.o

echo "Built ${OUT_DIR}/libclang_rt.profile-arm.a"
file "${OUT_DIR}/libclang_rt.profile-arm.a"
"${AR}" t "${OUT_DIR}/libclang_rt.profile-arm.a" | head -5
nm "${OUT_DIR}/libclang_rt.profile-arm.a" 2>/dev/null | grep -E '__llvm_profile|__llvm_write' | head -10
