#!/bin/sh
# Persistent content_shell for Berry Proxy (avoids ~30s cold start per page).
# Protocol on 127.0.0.1:8767 (TCP):  LOAD <url>   QUIT
# Protocol on stdout: @BERRY DOM <bytes>\n<html>\n@BERRY END
#
# Usage: ./run-daemon.sh [EXTRA_FLAGS...]

DIR="$(cd "$(dirname "$0")" && pwd)"
export LD_LIBRARY_PATH="$DIR:$LD_LIBRARY_PATH"
export QNX_BERRY_DAEMON=1
export QNX_BERRY_DAEMON_MARKER="$DIR/berry-daemon.mode"
touch "$QNX_BERRY_DAEMON_MARKER"
export QNX_BERRY_CMD_PORT="${QNX_BERRY_CMD_PORT:-8767}"

if [ -f "$DIR/cacert.pem" ]; then
  export SSL_CERT_FILE="$DIR/cacert.pem"
fi

TIER="${QNX_HARDENING_TIER:-0}"

DISABLED_FEATURES="ServiceWorker,NetworkServiceDedicatedThread,MojoIpcz,Viz"
case "$TIER" in
  0) ;;
  1) DISABLED_FEATURES="ServiceWorker,NetworkServiceDedicatedThread,MojoIpcz,Viz" ;;
  2) DISABLED_FEATURES="ServiceWorker,NetworkServiceDedicatedThread,Viz" ;;
  3) DISABLED_FEATURES="ServiceWorker,Viz" ;;
  4) DISABLED_FEATURES="ServiceWorker" ;;
  5) DISABLED_FEATURES="" ;;
  *) echo "Unknown QNX_HARDENING_TIER=$TIER (see HARDENING.md)" >&2; exit 1 ;;
esac

CERT_FLAGS="--ignore-certificate-errors"

HTTP2_FLAGS="--disable-http2"
if [ "$TIER" -ge 1 ] && [ -n "$SSL_CERT_FILE" ]; then
  HTTP2_FLAGS=""
fi

OZONE_FLAGS="--ozone-platform=headless --headless"
GPU_FLAGS="--disable-gpu --disable-gpu-compositing"
if [ "$TIER" -ge 4 ]; then
  OZONE_FLAGS="--ozone-platform=qnx_screen"
  GPU_FLAGS="--disable-gpu-compositing"
fi
if [ "$TIER" -ge 5 ]; then
  GPU_FLAGS=""
fi

PROC_FLAGS="--single-process"
if [ "$TIER" -ge 3 ]; then
  PROC_FLAGS=""
fi

FEATURE_FLAG=""
if [ -n "$DISABLED_FEATURES" ]; then
  FEATURE_FLAG="--disable-features=$DISABLED_FEATURES"
fi

EXTRA_FLAGS="$*"

exec "$DIR/content_shell" \
  --no-sandbox \
  --no-zygote \
  $PROC_FLAGS \
  $GPU_FLAGS \
  $FEATURE_FLAG \
  $OZONE_FLAGS \
  --berry-daemon \
  --dump-dom \
  --timeout=90000 \
  $HTTP2_FLAGS \
  $CERT_FLAGS \
  $EXTRA_FLAGS \
  about:blank
