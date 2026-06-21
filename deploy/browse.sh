#!/bin/sh
# Simple on-device browser launcher for BlackBerry 10 (QNX ARM).
# Renders to the physical screen via qnx_screen, real fonts, real HTTPS certs.
# Touch the screen to interact (tap links, scroll). Relaunch to change URL.
#
# Usage:
#   ./browse.sh                         # opens google.com
#   ./browse.sh https://en.wikipedia.org
#   ./browse.sh https://example.com
#
# Stop: ./kill-content-shell.sh   (or  slay -9 content_shell)

DIR="$(cd "$(dirname "$0")" && pwd)"
export LD_LIBRARY_PATH="$DIR:$LD_LIBRARY_PATH"

URL="${1:-https://www.google.com/}"

# Clear any stale instance first.
killall content_shell 2>/dev/null
slay -9 content_shell 2>/dev/null
sleep 1

exec "$DIR/content_shell" \
  --no-sandbox \
  --no-zygote \
  --single-process \
  --disable-gpu \
  --disable-gpu-compositing \
  --ozone-platform=qnx_screen \
  "$URL"
