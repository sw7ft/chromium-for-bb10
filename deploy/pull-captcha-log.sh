#!/bin/bash
# Pull CAPTCHA/reCAPTCHA session logs from Passport for handoff to another agent.
# Usage:
#   ./deploy/pull-captcha-log.sh              # pull full filtered log
#   ./deploy/pull-captcha-log.sh --since-mark # only lines after last CAPTCHA SESSION START
#   ./deploy/pull-captcha-log.sh --live 120   # tail live for N seconds then save

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-/root/berry-captcha-session.log}"
DEVICE_LOG="/accounts/1000/shared/misc/berry-kbd.log"
FILTER='BerryShell: UA|BerryNav: (DecodeProbe|CaptchaResp|CaptchaBody|LoadURL|OnRecvResp|commit|URLReqStart|Timeout|SyntaxError|reCAPTCHA|sorry|429|recaptcha|BerryHdr|webdriver|FP:|touch|Input|click)|BerryShell:'

MODE="full"
LIVE_SEC=0
if [[ "${1:-}" == "--since-mark" ]]; then
  MODE="since"
  OUT="${2:-/root/berry-captcha-session.log}"
elif [[ "${1:-}" == "--live" ]]; then
  MODE="live"
  LIVE_SEC="${2:-60}"
  OUT="${3:-/root/berry-captcha-session.log}"
fi

if [[ "$MODE" == "live" ]]; then
  echo "Live capture ${LIVE_SEC}s -> $OUT (click CAPTCHA on device now)..."
  ssh passport "tail -f -n 0 $DEVICE_LOG" | tee "$OUT.raw" &
  TPID=$!
  sleep "$LIVE_SEC"
  kill "$TPID" 2>/dev/null || true
  grep -E "$FILTER" "$OUT.raw" > "$OUT" || true
  rm -f "$OUT.raw"
else
  TMP="$(mktemp)"
  ssh passport "cat $DEVICE_LOG" > "$TMP"
  if [[ "$MODE" == "since" ]]; then
    awk '/=== CAPTCHA SESSION START/{found=1; buf=""; next} {if(found) buf=buf $0 ORS} END{printf "%s", buf}' "$TMP" > "$TMP.since"
    mv "$TMP.since" "$TMP"
  fi
  grep -E "$FILTER" "$TMP" > "$OUT" || true
  rm -f "$TMP"
fi

LINES=$(wc -l < "$OUT")
echo "Saved $LINES lines -> $OUT"
echo "--- last 40 lines ---"
tail -40 "$OUT"
