#!/bin/sh
# Start Berry Proxy — headless Chromium backend for the BB10 stock browser.
#
# Usage:
#   BERRY_NODE=/path/to/node ./start-proxy.sh
#   ./start-proxy.sh              # auto-detect node (port 8765)
#
# In the BB10 browser open: http://127.0.0.1:8765/

DIR="$(cd "$(dirname "$0")" && pwd)"
export BERRY_DEPLOY="$DIR"
export LD_LIBRARY_PATH="$DIR:$LD_LIBRARY_PATH"

if [ -f "$DIR/cacert.pem" ]; then
  export SSL_CERT_FILE="$DIR/cacert.pem"
fi

NODE=""
if [ -n "$BERRY_NODE" ] && [ -x "$BERRY_NODE" ]; then
  NODE="$BERRY_NODE"
elif [ -n "$NODE_BIN" ] && [ -x "$NODE_BIN" ]; then
  NODE="$NODE_BIN"
else
  for candidate in \
      "$DIR/node/node" \
      "/accounts/1000/shared/misc/node/node" \
      "/accounts/devuser/node/node" \
      "/accounts/devuser/berry-deploy/node/node"; do
    if [ -x "$candidate" ]; then
      NODE="$candidate"
      break
    fi
  done
fi

if [ -z "$NODE" ] && command -v node >/dev/null 2>&1; then
  NODE="$(command -v node)"
fi

if [ -z "$NODE" ]; then
  echo "Node.js not found." >&2
  echo "Set BERRY_NODE to your Node 22 wrapper, e.g.:" >&2
  echo "  BERRY_NODE=/accounts/1000/shared/misc/node/node ./start-proxy.sh" >&2
  exit 1
fi

if [ ! -x "$DIR/content_shell" ]; then
  echo "content_shell not found in $DIR" >&2
  exit 1
fi

export BERRY_PROXY_PORT="${BERRY_PROXY_PORT:-8765}"
export BERRY_PROXY_HOST="${BERRY_PROXY_HOST:-127.0.0.1}"
export BERRY_PROXY_TIMEOUT_MS="${BERRY_PROXY_TIMEOUT_MS:-120000}"

echo "Berry Proxy on http://${BERRY_PROXY_HOST}:${BERRY_PROXY_PORT}/"
echo "Deploy: $DIR"
echo "Node:   $NODE"
exec "$NODE" "$DIR/proxy-server.js"
