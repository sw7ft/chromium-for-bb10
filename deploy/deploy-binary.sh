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

# The installed BerryShell app execs content_shell from its OWN native asset
# dir (see launcher.c -> /proc/self/exefile), NOT the berry-browser-bundle
# staging path used by browse.sh over SSH. Deploying only to the bundle means
# the tapped-from-the-homescreen app keeps running the old binary. Push to both.
APP_NATIVE="${PASSPORT_APP_NATIVE:-/accounts/1000/appdata/com.sw7ft.BerryShell.testDev__BerryShellf5089f54/app/native}"

echo "=== kill stray content_shell on device ==="
ssh "$REMOTE_HOST" 'killall content_shell 2>/dev/null; sleep 1; killall content_shell 2>/dev/null; slay -9 content_shell 2>/dev/null; rm -f '"$REMOTE_PATH"'/content_shell.new '"$APP_NATIVE"'/content_shell.new'

deploy_to() {
  dest_path="$1"
  dest_name="$2"  # actual runtime binary name at this location
  echo "=== scp $(basename "$BINARY") -> $dest_path/$dest_name.new ==="
  scp "$BINARY" "$REMOTE_HOST:$dest_path/$dest_name.new"
  echo "=== atomic rename in $dest_path ($dest_name) ==="
  ssh "$REMOTE_HOST" "cd '$dest_path' && mv -f '$dest_name.new' '$dest_name' && chmod +x '$dest_name' && echo deploy_ok"
}

# SSH/browse.sh staging path runs the plain 'content_shell'.
deploy_to "$REMOTE_PATH" "content_shell"
# The installed app's launcher (launcher.c) execs 'content_shell.exe' from the
# app native dir -- THAT is the binary the tapped-from-homescreen app runs.
deploy_to "$APP_NATIVE" "content_shell.exe"
