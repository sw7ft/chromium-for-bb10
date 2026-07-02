#!/bin/sh
# YouTube playback smoke test (no --dump-dom; runs ~SECS then exits).
# Mirrors BerryBrowserV3 launcher defaults (single-process, software GPU, SW on).
# Usage: ./test-youtube.sh [watch_url] [secs]
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
export LD_LIBRARY_PATH="$DIR:$LD_LIBRARY_PATH"
URL="${1:-https://m.youtube.com/watch?v=jNQXAC9IVRw}"
SECS="${2:-45}"
LOG="${3:-/tmp/youtube-test.log}"

"$DIR/kill-content-shell.sh" 2>/dev/null || true
rm -f "$LOG"

"$DIR/content_shell" \
  --no-sandbox --no-zygote --single-process \
  --disable-gpu --disable-gpu-compositing \
  --ozone-platform=qnx_screen \
  --use-mobile-user-agent \
  --autoplay-policy=no-user-gesture-required \
  --disable-accelerated-video-decode \
  --disable-features=Translate,OptimizationHints,MediaRouter,PreconnectToSearch \
  --enable-low-end-device-mode \
  --ignore-certificate-errors \
  --enable-logging=stderr \
  "$URL" >>"$LOG" 2>&1 &
PID=$!
sleep "$SECS"
kill "$PID" 2>/dev/null || true
"$DIR/kill-content-shell.sh" 2>/dev/null || true

echo "=== test-youtube.sh (${SECS}s) exit log=$LOG ==="
grep -iE 'googlevideo|youtubei|get_watch|videoplayback|MEDIA_ERR|MediaError|CONSOLE.*(error|video|play|decod|MC:)' "$LOG" | sed -n '1,60p'
echo "--- tail ---"
tail -15 "$LOG"
