#!/bin/bash
# Pull Passport log and PASS/FAIL the YouTube watch-shim smoke test.
# Requires: user opened BerryBrowserV3, tapped home YouTube tile, waited ~30s.
#
# Usage:
#   ./smoke-youtube-shim.sh
#   ./smoke-youtube-shim.sh passport 800000
set -e

HOST="${1:-passport}"
TAIL_BYTES="${2:-800000}"
LOG="/accounts/1000/shared/misc/berry-kbd.log"
OUT="${BERRY_SMOKE_LOG:-/tmp/berry-youtube-smoke.log}"

echo "=== YouTube watch-shim smoke test ==="
echo "Pulling tail -c ${TAIL_BYTES} from ${HOST}:${LOG} ..."
ssh -o ConnectTimeout=10 "$HOST" "tail -c ${TAIL_BYTES} ${LOG}" >"$OUT" 2>/dev/null || {
  echo "FAIL: could not pull device log from ${HOST}" >&2
  exit 2
}

echo "Log saved: $OUT ($(wc -l <"$OUT") lines)"

FAIL=0
REASON=""

if grep -aqE 'SyntaxError' "$OUT"; then
  FAIL=1
  REASON="JS SyntaxError in console"
elif grep -aq 'WatchShimJsAlive MISSING' "$OUT"; then
  FAIL=1
  REASON="js-alive tripwire fired (script did not execute within 5s)"
elif ! grep -aq 'WatchShim js-alive' "$OUT" && ! grep -aq 'WatchShimJsAlive OK' "$OUT"; then
  FAIL=1
  REASON="no js-alive beacon (WatchShim js-alive / WatchShimJsAlive OK)"
elif ! grep -aqE 'decBody=[1-9][0-9]*' "$OUT"; then
  FAIL=1
  REASON="no googlevideo decBody>0 (MP4 not fetched)"
fi

echo ""
echo "--- key lines ---"
grep -aE 'build [0-9]+|WatchShim|js-alive|JsAlive|SyntaxError|InnertubeSpoof|PlayerFormats|decBody=[1-9]|JsAlive MISSING' "$OUT" | tail -25 || true

echo ""
if [ "$FAIL" -eq 0 ]; then
  echo "PASS: js-alive + decBody>0"
  exit 0
fi

echo "FAIL: ${REASON}"
echo "Hint: install bar, force-kill app, home -> YouTube tile, wait 30s, re-run."
exit 1
