#!/bin/sh
# Regression suite for QNX hardening tiers.
# Usage: ./test-regression.sh [--fast|--google] [tier]
#   tier defaults to QNX_HARDENING_TIER or 0
#   --fast runs example.com only (quick dev iteration)
#   --google runs google.com only

DIR="$(cd "$(dirname "$0")" && pwd)"
MODE=""
if [ "$1" = "--fast" ] || [ "$1" = "--google" ]; then
  MODE="$1"
  shift
fi

TIER="${1:-${QNX_HARDENING_TIER:-0}}"
export QNX_HARDENING_TIER="$TIER"

case "$MODE" in
  --fast) URLS="https://example.com" ;;
  --google) URLS="https://www.google.com" ;;
  *)
    URLS="
https://example.com
https://www.google.com
https://en.wikipedia.org/wiki/QNX
"
    ;;
esac

PASS=0
FAIL=0

"$DIR/kill-content-shell.sh" 2>/dev/null

echo "=== QNX regression tier $TIER ==="

for url in $URLS; do
  "$DIR/kill-content-shell.sh" 2>/dev/null
  sleep 3

  name=$(echo "$url" | sed 's|https://||;s|www.||;s|/|_|g')
  out="/tmp/qnx_reg_${TIER}_${name}.out"
  trace="/tmp/qnx_reg_${TIER}_${name}.trace"

  echo "--- $url ---"
  if ! "$DIR/run.sh" "$url" --timeout=90000 >"$out" 2>"$trace"; then
    echo "FAIL (exit code) $url"
    FAIL=$((FAIL + 1))
    continue
  fi

  bytes=$(wc -c <"$out")
  bytes=${bytes##* }
  if [ "$bytes" -gt 100 ] 2>/dev/null; then
    echo "PASS $url ($bytes bytes)"
    PASS=$((PASS + 1))
  else
    echo "FAIL (empty) $url ($bytes bytes)"
    grep -E 'HardWatchdog|SIGSEGV|InvalidReq|bad lock' "$trace" 2>/dev/null | tail -3
    FAIL=$((FAIL + 1))
  fi
done

"$DIR/kill-content-shell.sh" 2>/dev/null

echo "=== tier $TIER: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
