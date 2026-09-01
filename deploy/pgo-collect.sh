#!/usr/bin/env bash
# Berry Browser PGO profile collection helper.
set -euo pipefail

RAW_DIR="${PGO_RAW_DIR:-/root/pgo-profiles/raw}"
OUT_PROFDATA="${PGO_PROFDATA:-/root/pgo-profiles/berry-qnx-arm.profdata}"
DEVICE_PGO="/accounts/1000/shared/misc/pgo"
DEVICE_MISC="/accounts/1000/shared/misc"

usage() {
  cat <<'EOF'
Usage:
  ./deploy/pgo-collect.sh setup [host]     Enable berry-pgo-collect.enable on device
  ./deploy/pgo-collect.sh workload         Print tapped workload checklist
  ./deploy/pgo-collect.sh pull [host]      Download .profraw from device
  ./deploy/pgo-collect.sh merge            Merge raw/*.profraw -> berry-qnx-arm.profdata

Default host: passport
EOF
}

pull_profiles() {
  local host="${1:-passport}"
  mkdir -p "${RAW_DIR}"
  echo "=== Pulling .profraw from ${host}:${DEVICE_PGO}/ ==="
  ssh "${host}" "mkdir -p ${DEVICE_PGO}" || true
  if ! scp "${host}:${DEVICE_PGO}/"*.profraw "${RAW_DIR}/" 2>/dev/null; then
    echo "No .profraw files found on device yet."
    exit 1
  fi
  ls -la "${RAW_DIR}/"*.profraw
}

merge_profiles() {
  shopt -s nullglob
  local files=("${RAW_DIR}/"*.profraw)
  if [ ${#files[@]} -eq 0 ]; then
    echo "No profraw in ${RAW_DIR}. Run: ./deploy/pgo-collect.sh pull"
    exit 1
  fi
  echo "=== Merging ${#files[@]} profraw -> ${OUT_PROFDATA} ==="
  llvm-profdata-17 merge -sparse "${files[@]}" -o "${OUT_PROFDATA}"
  llvm-profdata-17 show --summary "${OUT_PROFDATA}" | head -30
  echo "Done: ${OUT_PROFDATA}"
}

setup_device() {
  local host="${1:-passport}"
  echo "=== Enabling PGO collection on ${host} ==="
  ssh "${host}" "mkdir -p ${DEVICE_PGO} && touch ${DEVICE_MISC}/berry-pgo-collect.enable"
}

print_workload() {
  cat <<'EOF'

=== TAPPED WORKLOAD (Passport) ===

1. Deploy instrumented binary from out/qnx-arm-pgi + updated launcher
2. berry-pgo-collect.enable must exist (./deploy/pgo-collect.sh setup)
3. TAP Berry Browser icon (not SSH)
4. Idle home.html ~10s
5. Omnibar: search "blackberry passport"
6. Open en.wikipedia.org/wiki/BlackBerry_Passport — scroll 30s
7. Home -> YouTube tile — buffer ~15s
8. Swipe-up close OR Restart (profiles flush when collect marker set)

Repeat 3-8 two more times for warm-cache coverage.

Then:
  ./deploy/pgo-collect.sh pull
  ./deploy/pgo-collect.sh merge
  ./deploy/build-pgo-bar.sh

Avoid: Google burst searches, youtube.com/ home, reCAPTCHA flows.

EOF
}

CMD="${1:-workload}"
HOST="${2:-passport}"
case "${CMD}" in
  setup) setup_device "${HOST}" ;;
  pull) pull_profiles "${HOST}" ;;
  merge) merge_profiles ;;
  workload) print_workload ;;
  -h|--help|help) usage ;;
  *) echo "Unknown command: ${CMD}"; usage; exit 1 ;;
esac
