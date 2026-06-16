#!/bin/bash
# Atomic content_shell deploy to Passport. Never run regression in same command.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BINARY="${1:-$SRC_DIR/out/qnx-arm/content_shell}"
REMOTE="${PASSPORT_DEPLOY:-passport:/accounts/devuser/berry-deploy/berry-browser-bundle}"

if [ ! -f "$BINARY" ]; then
  echo "Missing binary: $BINARY" >&2
  exit 1
fi

REMOTE_HOST="${REMOTE%%:*}"
REMOTE_PATH="${REMOTE#*:}"

echo "=== kill stray content_shell on device ==="
ssh "$REMOTE_HOST" 'killall content_shell 2>/dev/null; sleep 1; killall content_shell 2>/dev/null; rm -f '"$REMOTE_PATH"'/content_shell.new'

echo "=== scp $(basename "$BINARY") -> content_shell.new ==="
scp "$BINARY" "$REMOTE_HOST:$REMOTE_PATH/content_shell.new"

echo "=== atomic rename ==="
ssh "$REMOTE_HOST" "cd '$REMOTE_PATH' && mv content_shell.new content_shell && chmod +x content_shell && echo deploy_ok"
