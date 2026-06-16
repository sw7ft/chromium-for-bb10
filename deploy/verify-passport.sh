#!/bin/bash
# Deploy latest binary + scripts, then verify google and tier-0 regression.
# Run from build host when SSH to passport works (interactive password OK).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REMOTE="${PASSPORT_DEPLOY:-passport:/accounts/devuser/berry-deploy/berry-browser-bundle}"
REMOTE_HOST="${REMOTE%%:*}"
REMOTE_PATH="${REMOTE#*:}"

echo "=== deploy binary (atomic) ==="
"$SCRIPT_DIR/deploy-binary.sh"

echo "=== deploy scripts ==="
scp "$SCRIPT_DIR/run.sh" \
    "$SCRIPT_DIR/test-regression.sh" \
    "$SCRIPT_DIR/kill-content-shell.sh" \
    "$SCRIPT_DIR/HARDENING.md" \
    "$REMOTE_HOST:$REMOTE_PATH/"

ssh "$REMOTE_HOST" "chmod +x '$REMOTE_PATH'/*.sh"

echo "=== google alone ==="
ssh "$REMOTE_HOST" "cd '$REMOTE_PATH' && \
  ./kill-content-shell.sh && \
  ./run.sh https://www.google.com --timeout=90000 --qnx-trace >/tmp/g.out 2>/tmp/g.trace; \
  echo bytes=\$(wc -c </tmp/g.out); \
  grep -E 'StartTimer|FLFP:DumpDom|HardWatchdog' /tmp/g.trace | tail -5"

echo "=== tier 0 regression ==="
ssh "$REMOTE_HOST" "cd '$REMOTE_PATH' && ./test-regression.sh 0"
