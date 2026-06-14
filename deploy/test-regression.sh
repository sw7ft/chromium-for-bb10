#!/bin/sh
# Regression suite for QNX hardening tiers.
# Usage: ./test-regression.sh [tier]
#   tier defaults to QNX_HARDENING_TIER or 0

DIR="$(cd "$(dirname "$0")" && pwd)"
TIER="${1:-${QNX_HARDENING_TIER:-0}}"
export QNX_HARDENING_TIER="$TIER"

URLS="
https://example.com
https://www.google.com
https://en.wikipedia.org/wiki/QNX
"

PASS=0
FAIL=0

echo "=== QNX regression tier $TIER ==="

for url in $URLS; do
  name=$(echo "$url" | sed 's|https://||;s|www.||;s|/|_|g')
  out="/tmp/qnx_reg_${TIER}_${name}.out"
  trace="/tmp/qnx_reg_${TIER}_${name}.trace"

  echo "--- $url ---"
  if ! "$DIR/run.sh" "$url" --timeout=90000 --qnx-trace >"$out" 2>"$trace"; then
    echo "FAIL (exit code) $url"
    FAIL=$((FAIL + 1))
    continue
  fi

  bytes=$(wc -c <"$out" | tr -d ' ')
  if [ "$bytes" -gt 100 ] 2>/dev/null; then
    echo "PASS $url ($bytes bytes)"
    PASS=$((PASS + 1))
  else
    echo "FAIL (empty) $url ($bytes bytes)"
    grep -E 'HardWatchdog|SIGSEGV|InvalidReq|bad lock' "$trace" 2>/dev/null | tail -3
    FAIL=$((FAIL + 1))
  fi
done

echo "=== tier $TIER: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
