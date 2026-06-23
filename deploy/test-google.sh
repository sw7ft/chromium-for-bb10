#!/bin/bash
# Automated Google load/search test for Berry Browser on Passport.
# Agent-friendly: kill → clear log → launch → poll log → report pass/fail.
#
# Usage:
#   ./test-google.sh                 # interactive (launcher + qnx_screen)
#   ./test-google.sh --headless      # SSH bundle headless (fast)
#   ./test-google.sh --search        # interactive search?q=hi
#   ./test-google.sh --headless --search
#   ./test-google.sh --skip-cksum    # skip local vs remote cksum check
#   ./test-google.sh --no-deploy     # skip deploy-binary.sh
#
# Exit codes: 0=pass 1=crash 2=timeout 3=cksum 4=url-corrupt 5=deploy-fail

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
LOCAL_BIN="${LOCAL_BIN:-$SRC_DIR/out/qnx-arm/content_shell}"
SSH_HOST="${PASSPORT_SSH:-passport}"
REMOTE="${PASSPORT_DEPLOY:-passport:/accounts/devuser/berry-deploy/berry-browser-bundle}"
REMOTE_HOST="${REMOTE%%:*}"
REMOTE_PATH="${REMOTE#*:}"
APP_NATIVE="${PASSPORT_APP_NATIVE:-/accounts/1000/appdata/com.sw7ft.BerryShell.testDev__BerryShellf5089f54/app/native}"
APP_DATA="${PASSPORT_APP_DATA:-/accounts/1000/appdata/com.sw7ft.BerryShell.testDev__BerryShellf5089f54/data}"
KBDLOG='/accounts/1000/shared/misc/berry-kbd.log'
NAVDEBUG='/accounts/1000/shared/misc/berry-nav.debug'
TESTLOG='/accounts/1000/shared/misc/berry-test.log'

MODE=headless
URL='https://www.google.com/'
DO_SEARCH=0
SKIP_CKSUM=0
NO_DEPLOY=0
TIMEOUT_SEC="${TEST_GOOGLE_TIMEOUT:-120}"
GRACE_SEC="${TEST_GOOGLE_GRACE:-30}"
POLL_SEC=3

while [ $# -gt 0 ]; do
  case "$1" in
    --headless) MODE=headless; shift ;;
    --interactive) MODE=interactive; shift ;;
    --search) DO_SEARCH=1; shift ;;
    --skip-cksum) SKIP_CKSUM=1; shift ;;
    --no-deploy) NO_DEPLOY=1; shift ;;
    --timeout=*) TIMEOUT_SEC="${1#*=}"; shift ;;
    --grace=*) GRACE_SEC="${1#*=}"; shift ;;
    -h|--help)
      sed -n '2,14p' "$0"
      exit 0
      ;;
    *) echo "Unknown arg: $1" >&2; exit 2 ;;
  esac
done

if [ "$DO_SEARCH" -eq 1 ]; then
  URL='https://www.google.com/search?q=hi'
fi

MP_KILL='killall content_shell 2>/dev/null || true; killall content_shell.exe 2>/dev/null || true; slay -9 content_shell 2>/dev/null || true; slay -9 content_shell.exe 2>/dev/null || true; slay -9 launcher 2>/dev/null || true; sleep 2; killall content_shell 2>/dev/null || true; killall content_shell.exe 2>/dev/null || true; slay -9 content_shell 2>/dev/null || true; slay -9 content_shell.exe 2>/dev/null || true'

cleanup() {
  ssh -o ConnectTimeout=8 "$SSH_HOST" "$MP_KILL" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

fail() {
  echo "FAIL: $*" >&2
  exit "${2:-1}"
}

echo "=== test-google mode=$MODE url=$URL timeout=${TIMEOUT_SEC}s grace=${GRACE_SEC}s ==="

if [ ! -f "$LOCAL_BIN" ]; then
  fail "missing local binary: $LOCAL_BIN" 5
fi

if [ "$NO_DEPLOY" -eq 0 ]; then
  echo "=== deploy binary ==="
  if ! "$SCRIPT_DIR/deploy-binary.sh" "$LOCAL_BIN"; then
    fail "deploy-binary.sh failed" 5
  fi
fi

LOCAL_CKSUM=$(cksum "$LOCAL_BIN" | awk '{print $1" "$2}')
REMOTE_CKSUM=$(ssh -o ConnectTimeout=10 "$SSH_HOST" \
  "cksum '$APP_NATIVE/content_shell.exe' 2>/dev/null" | awk '{print $1" "$2}')

echo "local  cksum: $LOCAL_CKSUM"
echo "remote cksum: $REMOTE_CKSUM"

if [ "$SKIP_CKSUM" -eq 0 ]; then
  if [ -z "$REMOTE_CKSUM" ] || [ "$LOCAL_CKSUM" != "$REMOTE_CKSUM" ]; then
    fail "cksum mismatch — device may be running stale binary" 3
  fi
  echo "cksum OK"
fi

echo "=== prepare device ==="
ssh -o ConnectTimeout=10 "$SSH_HOST" "
  $MP_KILL
  cp /dev/null '$KBDLOG'
  cp /dev/null '$TESTLOG'
  touch '$NAVDEBUG'
  rm -f '$REMOTE_PATH/berry-daemon.mode'
  rm -f '$APP_DATA/berry-daemon.mode'
"

echo "=== launch ($MODE) ==="
if [ "$MODE" = headless ]; then
  ssh -o ConnectTimeout=10 "$SSH_HOST" "
    cd '$REMOTE_PATH'
    export LD_LIBRARY_PATH='$REMOTE_PATH' QNX_NAV_DEBUG=1
    [ -f cacert.pem ] && export SSL_CERT_FILE='$REMOTE_PATH/cacert.pem'
    killall content_shell 2>/dev/null; slay -9 content_shell 2>/dev/null; sleep 1
    ./content_shell \
      --no-sandbox --no-zygote --single-process \
      --disable-gpu --disable-gpu-compositing \
      --ozone-platform=headless --headless \
      --ignore-certificate-errors \
      --disable-features=ServiceWorker,NetworkServiceDedicatedThread,MojoIpcz \
      '$URL' >>'$TESTLOG' 2>&1 &
    echo launched_headless_pid=\$!
  "
else
  ssh -o ConnectTimeout=10 "$SSH_HOST" "
    cd '$APP_DATA'
    export HOME='$APP_DATA' LD_LIBRARY_PATH='$APP_NATIVE' QNX_NAV_DEBUG=1
    '$APP_NATIVE/launcher' '$URL' >>'$TESTLOG' 2>&1 &
    echo launched_interactive_pid=\$!
  "
fi

echo "=== poll log (timeout ${TIMEOUT_SEC}s, grace ${GRACE_SEC}s after nav) ==="
START=$(date +%s)
NAV_OK=0
NAV_TS=0
RESULT=2

while true; do
  NOW=$(date +%s)
  ELAPSED=$((NOW - START))

  LOG_TAIL=$(ssh -o ConnectTimeout=10 "$SSH_HOST" "
    { grep -E 'BerryNav|QNX:CRASH|SIGSEGV|DidFinishNavigation|DidFinishLoad|QNX:Shell:DFL|QNX:RFI:DidFinishLoad|LoadURL' '$KBDLOG' 2>/dev/null;
      grep -E 'BerryNav|QNX:CRASH|SIGSEGV|DidFinishNavigation|DidFinishLoad|QNX:Shell:DFL|QNX:RFI:DidFinishLoad|LoadURL' '$TESTLOG' 2>/dev/null; } | tail -25
  " || true)

  if echo "$LOG_TAIL" | grep -q 'QNX:CRASH'; then
    echo "--- crash detected (${ELAPSED}s) ---"
    echo "$LOG_TAIL"
    fail "SIGSEGV during load" 1
  fi

  if [ "$DO_SEARCH" -eq 1 ]; then
    FIRST_NAV=$(echo "$LOG_TAIL" | grep 'DidFinishNavigation' | grep 'search' | head -1 || true)
    if [ -n "$FIRST_NAV" ] && echo "$FIRST_NAV" | grep -q 'search%3Fq'; then
      echo "--- URL corruption in first search nav (%3Fq) ---"
      echo "$FIRST_NAV"
      fail "first search nav has encoded ? in path (search%3Fq)" 4
    fi
    if echo "$LOG_TAIL" | grep 'DidFinishNavigation' | grep -q 'search?q='; then
      if [ "$NAV_OK" -eq 0 ]; then
        NAV_OK=1
        NAV_TS=$NOW
        echo "nav OK (search) at ${ELAPSED}s"
        echo "$LOG_TAIL" | grep 'DidFinishNavigation' | tail -1
      fi
    fi
  else
    if echo "$LOG_TAIL" | grep -E 'DidFinishNavigation|QNX:RFI:DidFinishLoad' | grep -q 'google.com'; then
      if echo "$LOG_TAIL" | grep 'DidFinishNavigation' | grep -q 'error=0 code=0'; then
        if [ "$NAV_OK" -eq 0 ]; then
          NAV_OK=1
          NAV_TS=$NOW
          echo "nav OK at ${ELAPSED}s"
          echo "$LOG_TAIL" | grep -E 'DidFinishNavigation|QNX:RFI:DidFinishLoad' | tail -1
        fi
      elif echo "$LOG_TAIL" | grep -q 'QNX:RFI:DidFinishLoad'; then
        if [ "$NAV_OK" -eq 0 ]; then
          NAV_OK=1
          NAV_TS=$NOW
          echo "nav OK (RFI DFL) at ${ELAPSED}s"
          echo "$LOG_TAIL" | grep 'QNX:RFI:DidFinishLoad' | tail -1
        fi
      fi
    fi
  fi

  if [ "$NAV_OK" -eq 1 ]; then
    GRACE_ELAPSED=$((NOW - NAV_TS))
    if [ "$GRACE_ELAPSED" -ge "$GRACE_SEC" ]; then
      echo "PASS: nav committed, no crash within ${GRACE_SEC}s grace (${ELAPSED}s total)"
      echo "--- final log tail ---"
      echo "$LOG_TAIL"
      exit 0
    fi
    echo "  grace ${GRACE_ELAPSED}/${GRACE_SEC}s (total ${ELAPSED}s)..."
  else
    echo "  waiting for nav... ${ELAPSED}s"
  fi

  if [ "$ELAPSED" -ge "$TIMEOUT_SEC" ]; then
    echo "--- timeout log tail ---"
    echo "$LOG_TAIL"
    fail "timeout waiting for navigation (${TIMEOUT_SEC}s)" 2
  fi

  sleep "$POLL_SEC"
done
