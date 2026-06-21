#!/bin/bash
# One-shot multi-process debug test. Always slays on exit. No pidin, no loops.
set -euo pipefail

APP='/apps/com.sw7ft.BerryShell.testDev__BerryShellf5089f54/native'
DATA='/accounts/1000/appdata/com.sw7ft.BerryShell.testDev__BerryShellf5089f54/data'
MPLOG='/accounts/1000/shared/misc/berry-mp-debug.log'
KBDLOG='/accounts/1000/shared/misc/berry-kbd.log'
SSH_HOST="${PASSPORT_SSH:-passport}"
WAIT_SEC="${MP_TEST_WAIT:-20}"

MP_KILL='killall content_shell 2>/dev/null; killall content_shell.exe 2>/dev/null; slay -9 content_shell 2>/dev/null; slay -9 content_shell.exe 2>/dev/null; slay -9 launcher 2>/dev/null; sleep 2; killall content_shell 2>/dev/null; killall content_shell.exe 2>/dev/null; slay -9 content_shell 2>/dev/null; slay -9 content_shell.exe 2>/dev/null; slay -9 launcher 2>/dev/null; sleep 2; killall content_shell 2>/dev/null; killall content_shell.exe 2>/dev/null; slay -9 content_shell 2>/dev/null; slay -9 content_shell.exe 2>/dev/null'

cleanup() {
  ssh -o ConnectTimeout=8 "$SSH_HOST" "$MP_KILL" || true
}
trap cleanup EXIT

echo "=== pre-kill, launch once, read logs, kill loop (single SSH) ==="
ssh -o ConnectTimeout=10 "$SSH_HOST" "
  $MP_KILL
  : > '$MPLOG'
  cd '$DATA'
  export HOME='$DATA' LD_LIBRARY_PATH='$APP'
  '$APP/launcher' >/dev/null 2>&1 &
  LPID=\$!
  sleep $WAIT_SEC
  echo '--- mp-debug ---'
  cat '$MPLOG'
  echo '--- kbd tail ---'
  tail -8 '$KBDLOG'
  # Kill launcher first so it cannot respawn children, then sweep all content_shell.
  slay -9 launcher 2>/dev/null
  kill -9 \$LPID 2>/dev/null
  $MP_KILL
  echo done
"

echo "=== done (cleanup runs via trap) ==="
