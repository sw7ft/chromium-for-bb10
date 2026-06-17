#!/bin/sh
# Capture all-thread stacks of a hung content_shell via /proc (qnx_stack),
# which works on signal-masked threads (e.g. the GPU/Viz init thread).
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR" || exit 1
export LD_LIBRARY_PATH="$DIR:$LD_LIBRARY_PATH"
[ -f "$DIR/cacert.pem" ] && export SSL_CERT_FILE="$DIR/cacert.pem"
# Long window so the hung process stays parked while we read it.
export QNX_BOOT_WATCHDOG_MS=120000

OUT=/tmp/stacks.txt
: > "$OUT"

attempt=0
while [ $attempt -lt 10 ]; do
  attempt=$((attempt+1))
  killall content_shell 2>/dev/null
  slay -9 content_shell 2>/dev/null
  sleep 1
  rm -f /tmp/c.out /tmp/c.err
  ./content_shell \
    --no-sandbox --no-zygote --single-process \
    --disable-gpu --disable-gpu-compositing \
    --ozone-platform=headless --headless \
    --dump-dom --ignore-certificate-errors \
    https://example.com >/tmp/c.out 2>/tmp/c.err &
  PID=$!
  # Healthy runs finish (DOM dumped) within ~30s. Wait past that.
  sleep 40
  B=$(wc -c </tmp/c.out)
  if kill -0 "$PID" 2>/dev/null && [ "$B" -eq 0 ]; then
    echo "HUNG attempt=$attempt pid=$PID" | tee -a "$OUT"
    t=1
    while [ $t -le 32 ]; do
      echo "=== tid $t ===" >> "$OUT"
      ./qnx_stack "$PID" "$t" >> "$OUT" 2>&1
      t=$((t+1))
    done
    killall content_shell 2>/dev/null
    slay -9 content_shell 2>/dev/null
    echo "DONE: stacks in $OUT"
    exit 0
  fi
  echo "attempt=$attempt not hung (bytes=$B), retry"
  killall content_shell 2>/dev/null
  slay -9 content_shell 2>/dev/null
  sleep 1
done
echo "no hang captured in $attempt attempts"
exit 2
