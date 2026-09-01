#!/bin/sh
# Start the Berry Remote screen-mirror service on the BB10/QNX device.
# Persists after the launching SSH session closes WITHOUT nohup/setsid (which
# busybox lacks here) by setting SIGHUP to ignore before launching node -- an
# ignored signal disposition is inherited across fork/exec, so node (and the
# content_shell it spawns) survive the SSH disconnect.
#
#   PORT=8080 URL=https://web.whatsapp.com/ sh remote-start.sh
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR" || exit 1

PORT="${PORT:-8080}"
URL="${URL:-https://web.whatsapp.com/}"
RENDER="${RENDER:-720}"
# Default to the node runtime shipped beside this bundle (../node/node),
# falling back to the legacy berry-deploy location.
if [ -z "$NODE" ]; then
  if [ -x "$DIR/../node/node" ]; then NODE="$DIR/../node/node";
  else NODE="/accounts/devuser/berry-deploy/node/node"; fi
fi
LOG="$DIR/berry-remote.log"

# One instance at a time: drop the run marker first so any previous
# supervisor loop exits instead of respawning what we slay.
rm -f "$DIR/.remote-run"
slay -f content_shell 2>/dev/null
slay -f node 2>/dev/null
sleep 1
: > "$LOG"

trap '' HUP
# --jitless is REQUIRED: the experimental QNX ARM32 node build's JIT emits
# broken code — once any function gets hot enough to tier up, the process
# dies with SIGBUS at a fixed address. Interpreter-only is stable, and this
# service is I/O-bound so the speed cost doesn't matter.
#
# Supervisor loop: the service exits whenever the engine dies (content_shell
# can still hit fatal CHECKs on this port), so respawn until remote-stop.sh
# removes the run marker.
NODE_DIR="$(dirname "$NODE")"
# The marker holds this supervisor's nonce: a stale supervisor loop from an
# older start sees a foreign nonce and exits instead of fighting for the port.
NONCE="$$.$(date +%s 2>/dev/null || echo 0)"
echo "$NONCE" > "$DIR/.remote-run"
(
  while [ "$(cat "$DIR/.remote-run" 2>/dev/null)" = "$NONCE" ]; do
    PORT="$PORT" URL="$URL" RENDER="$RENDER" LD_LIBRARY_PATH="$NODE_DIR:$LD_LIBRARY_PATH" \
      "$NODE" --jitless "$DIR/berry-remote.js" >> "$LOG" 2>&1
    echo "[supervisor] service exited rc=$? $(date) — respawning in 3s" >> "$LOG"
    sleep 3
  done
  echo "[supervisor] run marker removed, stopping $(date)" >> "$LOG"
) < /dev/null > /dev/null 2>&1 &
PID=$!
echo "berry-remote started pid=$PID port=$PORT url=$URL render=$RENDER"
echo "log: $LOG"
echo "open: http://<device-ip>:$PORT   (this device: try http://192.168.1.107:$PORT)"
