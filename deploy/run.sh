#!/bin/sh
# Chromium content_shell launcher for BlackBerry 10 (QNX ARM32)
# Usage: ./run.sh [URL] [OPTIONS]
#   ./run.sh                                           # data: URL hello world
#   ./run.sh http://example.com                        # external HTTP
#   ./run.sh https://example.com                       # external HTTPS
#   ./run.sh http://127.0.0.1:8001/                    # local HTTP
#   ./run.sh 'data:text/html,<b>Hi</b>'               # inline HTML
#   ./run.sh https://www.google.com --timeout=30000    # 30s timeout for JS-heavy pages
#   ./run.sh http://example.com --dom-trigger=domcontentloaded  # dump on DOMContentLoaded

DIR="$(cd "$(dirname "$0")" && pwd)"
export LD_LIBRARY_PATH="$DIR:$LD_LIBRARY_PATH"

if [ -f "$DIR/cacert.pem" ]; then
  export SSL_CERT_FILE="$DIR/cacert.pem"
fi

URL="${1:-data:text/html,<h1>Hello from BB10</h1>}"
shift 2>/dev/null
EXTRA_FLAGS="$*"

CERT_FLAGS=""
if [ -z "$SSL_CERT_FILE" ]; then
  CERT_FLAGS="--ignore-certificate-errors"
fi

exec "$DIR/content_shell" \
  --no-sandbox \
  --disable-gpu \
  --disable-gpu-compositing \
  --no-zygote \
  --single-process \
  --disable-features=ServiceWorker,NetworkServiceDedicatedThread,MojoIpcz,Viz \
  --ozone-platform=headless \
  --headless \
  --dump-dom \
  --disable-http2 \
  $CERT_FLAGS \
  $EXTRA_FLAGS \
  "$URL"
