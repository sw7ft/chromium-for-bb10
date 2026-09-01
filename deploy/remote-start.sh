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
NODE="${NODE:-/accounts/devuser/berry-deploy/node/node}"
LOG="$DIR/berry-remote.log"

# One instance at a time.
killall content_shell 2>/dev/null
killall node.bin 2>/dev/null
sleep 1

trap '' HUP
PORT="$PORT" URL="$URL" RENDER="$RENDER" "$NODE" "$DIR/berry-remote.js" > "$LOG" 2>&1 &
PID=$!
echo "berry-remote started pid=$PID port=$PORT url=$URL render=$RENDER"
echo "log: $LOG"
echo "open: http://<device-ip>:$PORT   (this device: try http://192.168.1.107:$PORT)"
