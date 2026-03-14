#!/bin/sh
# Chromium content_shell launcher for BlackBerry 10 (QNX ARM32)
# Usage: ./run.sh [URL]
#   ./run.sh                              # data: URL hello world
#   ./run.sh http://example.com           # external HTTP
#   ./run.sh http://127.0.0.1:8001/       # local HTTP
#   ./run.sh 'data:text/html,<b>Hi</b>'   # inline HTML

DIR="$(cd "$(dirname "$0")" && pwd)"
export LD_LIBRARY_PATH="$DIR:$LD_LIBRARY_PATH"

URL="${1:-data:text/html,<h1>Hello from BB10</h1>}"

exec "$DIR/content_shell" \
  --no-sandbox \
  --disable-gpu \
  --no-zygote \
  --single-process \
  --disable-features=ServiceWorker,NetworkServiceDedicatedThread,MojoIpcz \
  --ozone-platform=headless \
  --headless \
  --dump-dom \
  "$URL"
