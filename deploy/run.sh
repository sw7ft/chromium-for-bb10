#!/bin/sh
# Chromium content_shell launcher for BlackBerry 10 (QNX ARM32)
# Usage: ./run.sh [URL] [OPTIONS]
#   ./run.sh                                           # data: URL hello world
#   ./run.sh http://example.com                        # external HTTP
#   ./run.sh https://example.com                       # external HTTPS
#   QNX_HARDENING_TIER=1 ./run.sh https://example.com  # tiered re-enablement
#
# See HARDENING.md for tier definitions.

DIR="$(cd "$(dirname "$0")" && pwd)"
export LD_LIBRARY_PATH="$DIR:$LD_LIBRARY_PATH"

cleanup() {
  "$DIR/kill-content-shell.sh" 2>/dev/null
}
trap cleanup EXIT INT TERM

if [ -f "$DIR/cacert.pem" ]; then
  export SSL_CERT_FILE="$DIR/cacert.pem"
fi

URL="${1:-data:text/html,<h1>Hello from BB10</h1>}"
shift 2>/dev/null
EXTRA_FLAGS="$*"

# QNX /bin/sh mishandles --qnx-trace in expanded flag lists (exit 126); use env.
case " $EXTRA_FLAGS " in
  *" --qnx-trace "*) export QNX_TRACE=1; EXTRA_FLAGS=$(echo "$EXTRA_FLAGS" | sed 's/--qnx-trace//g') ;;
esac

rm -f "$DIR/berry-daemon.mode"

TIER="${QNX_HARDENING_TIER:-0}"

# Features disabled at tier 0; drop one-by-one as higher tiers pass regression.
DISABLED_FEATURES="ServiceWorker,NetworkServiceDedicatedThread,MojoIpcz,Viz"
case "$TIER" in
  0) ;;
  1) DISABLED_FEATURES="ServiceWorker,NetworkServiceDedicatedThread,MojoIpcz,Viz" ;;
  2) DISABLED_FEATURES="ServiceWorker,NetworkServiceDedicatedThread,Viz" ;;  # +MojoIpcz
  3) DISABLED_FEATURES="ServiceWorker,Viz" ;;                              # +NetworkServiceDedicatedThread
  4) DISABLED_FEATURES="ServiceWorker" ;;                                    # +Viz
  5) DISABLED_FEATURES="" ;;                                                 # +ServiceWorker
  *) echo "Unknown QNX_HARDENING_TIER=$TIER (see HARDENING.md)" >&2; exit 1 ;;
esac

CERT_FLAGS="--ignore-certificate-errors"
# SSL_CERT_FILE is exported when cacert.pem is present for future use; QNX
# still requires --ignore-certificate-errors until the platform trust store
# is wired up.

# Tier 1+: allow HTTP/2 when CA bundle is present
HTTP2_FLAGS="--disable-http2"
if [ "$TIER" -ge 1 ] && [ -n "$SSL_CERT_FILE" ]; then
  HTTP2_FLAGS=""
fi

# Tier 4+: on-screen qnx_screen instead of headless
OZONE_FLAGS="--ozone-platform=headless --headless"
GPU_FLAGS="--disable-gpu --disable-gpu-compositing"
if [ "$TIER" -ge 4 ]; then
  OZONE_FLAGS="--ozone-platform=qnx_screen"
  GPU_FLAGS="--disable-gpu-compositing"
fi
if [ "$TIER" -ge 5 ]; then
  GPU_FLAGS=""
fi

# Tier 3+: multi-process (still no zygote on QNX)
PROC_FLAGS="--single-process"
if [ "$TIER" -ge 3 ]; then
  PROC_FLAGS=""
fi

FEATURE_FLAG=""
if [ -n "$DISABLED_FEATURES" ]; then
  FEATURE_FLAG="--disable-features=$DISABLED_FEATURES"
fi

"$DIR/content_shell" \
  --no-sandbox \
  --no-zygote \
  $PROC_FLAGS \
  $GPU_FLAGS \
  $FEATURE_FLAG \
  $OZONE_FLAGS \
  --dump-dom \
  $HTTP2_FLAGS \
  $CERT_FLAGS \
  $EXTRA_FLAGS \
  "$URL"
exit $?
