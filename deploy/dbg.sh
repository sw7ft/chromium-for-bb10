#!/bin/sh
# On-device debug launcher: content_shell with remote-debugging on :9222,
# software render (stable), logging to /tmp/cs.log. Matches the installed app's
# flag set as closely as possible (minus GPU, which needs the launcher's screen
# setup). Arg1=URL, Arg2=extra flags (optional).
B=/accounts/devuser/berry-deploy/berry-browser-bundle
killall content_shell.exe 2>/dev/null; killall content_shell 2>/dev/null
slay -9 content_shell.exe 2>/dev/null; slay -9 content_shell 2>/dev/null
sleep 1
cd "$B"
export LD_LIBRARY_PATH="$B:$LD_LIBRARY_PATH"
URL="${1:-https://www.google.com/}"
EXTRA="$2"
./content_shell \
  --no-sandbox --no-zygote --single-process \
  --disable-background-networking --disable-background-timer-throttling \
  --disable-renderer-backgrounding --disable-backgrounding-occluded-windows \
  --disable-client-side-phishing-detection --disable-default-apps \
  --disable-domain-reliability --disable-hang-monitor --disable-prompt-on-repost \
  --disable-sync --no-first-run --no-default-browser-check --disable-component-update \
  --ignore-certificate-errors --disable-quic --disable-renderer-accessibility \
  --disable-frame-rate-limit --disable-gpu --disable-gpu-compositing \
  --ozone-platform=qnx_screen --force-device-scale-factor=1 \
  --enable-low-end-device-mode --use-mobile-user-agent \
  --enable-logging=stderr --log-level=0 --autoplay-policy=no-user-gesture-required \
  --use-fake-ui-for-media-stream \
  --remote-debugging-port=9222 --remote-allow-origins=* \
  $EXTRA \
  "$URL" > /tmp/cs.log 2>&1 &
echo "launched pid=$! url=$URL"
