#!/usr/bin/env bash
# Compare PGO vs non-PGO Berry metrics from device berry-kbd.log snippets.
# Usage: ./deploy/validate-pgo-ab.sh baseline.log pgo.log
set -euo pipefail

if [ $# -lt 2 ]; then
  cat <<'EOF'
Usage: ./deploy/validate-pgo-ab.sh <baseline.log> <pgo.log>

Pull logs from Passport after tapped sessions with berry-fps.enable:
  scp passport:/accounts/1000/shared/misc/berry-kbd.log ./baseline.log
  # deploy PGO .bar, repeat workload, pull again as pgo.log

Compares QNX:BF (main-thread stalls), QNX:FPS, QNX:GLSWAP, binary size.
EOF
  exit 1
fi

BASE="$1"
PGO="$2"
CHROMIUM_SRC="$(cd "$(dirname "$0")/.." && pwd)"

metrics() {
  local label="$1" file="$2"
  echo "=== ${label} (${file}) ==="
  echo -n "  QNX:BF max delta (ms): "
  grep 'QNX:BF' "$file" 2>/dev/null | sed -n 's/.*delta=\([0-9.]*\).*/\1/p' | sort -n | tail -1 || echo "n/a"
  echo -n "  QNX:FPS present samples: "
  grep -c 'QNX:FPS present=' "$file" 2>/dev/null || echo 0
  echo -n "  QNX:GLSWAP count: "
  grep -c 'QNX:GLSWAP' "$file" 2>/dev/null || echo 0
  echo -n "  content_shell size: "
  if [ "$label" = "baseline" ]; then
    ls -la "${CHROMIUM_SRC}/out/qnx-arm/content_shell" 2>/dev/null | awk '{print $5 " bytes"}' || echo "n/a"
  else
    ls -la "${CHROMIUM_SRC}/out/qnx-arm-pgo/content_shell" 2>/dev/null | awk '{print $5 " bytes"}' || echo "n/a"
  fi
  echo
}

metrics "baseline" "$BASE"
metrics "PGO" "$PGO"

echo "Tip: lower QNX:BF max delta and higher FPS sample rate indicate improvement."
