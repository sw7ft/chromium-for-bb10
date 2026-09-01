#!/usr/bin/env bash
# Build BerryBrowserV3 .bar using PGO-optimized content_shell (out/qnx-arm-pgo).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHROMIUM_SRC="$(cd "${SCRIPT_DIR}/.." && pwd)"
PROFDATA="${PGO_PROFDATA:-/root/pgo-profiles/berry-qnx-arm.profdata}"
OUT_PGO="${CHROMIUM_SRC}/out/qnx-arm-pgo"

if [ ! -f "${PROFDATA}" ]; then
  echo "ERROR: Missing ${PROFDATA}"
  echo "Collect profiles first:"
  echo "  ninja -C out/qnx-arm-pgi content/shell:content_shell"
  echo "  deploy PGI binary + touch berry-pgo-collect.enable on device"
  echo "  ./deploy/pgo-collect.sh  # follow workload"
  echo "  ./deploy/pgo-collect.sh pull && ./deploy/pgo-collect.sh merge"
  exit 1
fi

echo "=== PGO build (out/qnx-arm-pgo) ==="
cd "${CHROMIUM_SRC}"
if [ ! -f "${OUT_PGO}/build.ninja" ]; then
  gn gen "${OUT_PGO}"
fi
ninja -C "${OUT_PGO}" content/shell:content_shell

echo "=== Package .bar with PGO binary ==="
# build-v3-bar.sh copies from out/qnx-arm by default; override via env.
export BERRY_CONTENT_SHELL="${OUT_PGO}/content_shell"
"${SCRIPT_DIR}/build-v3-bar.sh"

echo "PGO bar ready. Binary: ${OUT_PGO}/content_shell"
