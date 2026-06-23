#!/bin/sh
# Phase-1 wait baseline for Berry Browser on BB10.
# Samples thread states + hogs while content_shell runs with qnx_screen and
# auto-scroll. Writes a report to /accounts/1000/shared/misc/wait-profile.txt
# (or /tmp/wait-profile.txt when run from berry-deploy).
#
# Usage (on device):
#   ./wait-profile.sh [software|gpu] [seconds]
#
# Prereq: touch /accounts/1000/shared/misc/berry-fps.enable

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR" || exit 1
export LD_LIBRARY_PATH="$DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
[ -f "$DIR/cacert.pem" ] && export SSL_CERT_FILE="$DIR/cacert.pem"

MODE="${1:-software}"
DUR="${2:-12}"
REPORT="/accounts/1000/shared/misc/wait-profile.txt"
if [ ! -w "$(dirname "$REPORT")" ] 2>/dev/null; then
  REPORT="/tmp/wait-profile.txt"
fi

touch /accounts/1000/shared/misc/berry-fps.enable 2>/dev/null || true
rm -f "$REPORT"

GPU_FLAGS="--disable-gpu --disable-gpu-compositing"
case "$MODE" in
  gpu) GPU_FLAGS="--use-gl=egl" ;;
  software) ;;
  *) echo "mode must be software or gpu" >&2; exit 1 ;;
esac

URL='data:text/html,<style>body{margin:0}#s{height:12000px;background:linear-gradient(#eef,#336)}</style><div id=s></div><script>var y=0;setInterval(function(){y+=40;window.scrollTo(0,y%11000)},16)</script>'

killall content_shell 2>/dev/null || true
slay content_shell 2>/dev/null || true
sleep 1

LOG="$DIR/wait-profile-stderr.log"
: > "$LOG"

./content_shell \
  --no-sandbox --no-zygote --single-process \
  $GPU_FLAGS \
  --disable-features=ServiceWorker,NetworkServiceDedicatedThread,MojoIpcz,Translate,OptimizationHints,MediaRouter,PreconnectToSearch \
  --ozone-platform=qnx_screen \
  --ignore-certificate-errors \
  "$URL" >>"$LOG" 2>&1 &
PID=$!

{
  echo "=== wait-profile $(date) mode=$MODE dur=${DUR}s pid=$PID ==="
  echo "URL=$URL"
  echo ""
} >>"$REPORT"

# Warm up compositor, then sample.
sleep 3
t=0
while [ "$t" -lt "$DUR" ]; do
  if ! kill -0 "$PID" 2>/dev/null; then
    echo "PROCESS_EXITED t=${t}s" >>"$REPORT"
    break
  fi
  echo "--- sample t=${t}s ---" >>"$REPORT"
  pidin -p "$PID" 2>>"$REPORT" | awk '
    NR==1 { print; next }
    { st=$5; c[st]++; if (st=="RUNNING") r++; if (st=="REPLY") rep++; if (st=="CONDVAR") cv++; if (st=="SIGWAITINFO") sw++ }
    END {
      printf "STATE_COUNTS RUNNING=%d CONDVAR=%d SIGWAITINFO=%d REPLY=%d\n", r+0, cv+0, sw+0, rep+0
      for (s in c) printf "  %s=%d\n", s, c[s]
    }
  ' >>"$REPORT" 2>&1 || echo "pidin_failed" >>"$REPORT"
  hogs -p "$PID" 2>>"$REPORT" | awk 'NR<=5 {print "HOGS:", $0}' >>"$REPORT" 2>&1 || true
  sleep 1
  t=$((t + 1))
done

kill "$PID" 2>/dev/null || true
killall content_shell 2>/dev/null || true
slay content_shell 2>/dev/null || true
sleep 1

{
  echo ""
  echo "=== QNX:FPS lines ==="
  grep 'QNX:FPS' "$LOG" || echo "(none)"
  echo ""
  echo "=== QNX:SYNC wait (first 20) ==="
  grep 'QNX:SYNC:wait' "$LOG" | awk 'NR<=20' || echo "(none)"
  echo ""
  echo "=== QNX:DS:attempt needs_draw=0 (count) ==="
  nd0=$(grep -c 'needs_draw=0' "$LOG" 2>/dev/null || echo 0)
  nd1=$(grep -c 'needs_draw=1' "$LOG" 2>/dev/null || echo 0)
  echo "needs_draw=0: $nd0  needs_draw=1: $nd1"
  echo ""
  echo "=== tail stderr ==="
  tail -30 "$LOG"
} >>"$REPORT"

echo "Report: $REPORT"
cat "$REPORT"
