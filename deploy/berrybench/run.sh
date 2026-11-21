#!/bin/sh
# On-device bench runner for Berry Browser engines (runs over SSH as root).
#
#   run.sh <engine-file> <js-flags | -> <ozone: headless|qnx_screen> <url> [extra flags]
#
# Reproduces the flag set the shipped launcher (deploy/berry-shell-bar/
# launcher.c, build 110) passes for the default configuration, with two
# deliberate differences:
#   * software rendering (--disable-gpu --disable-gpu-compositing): BB10 only
#     hands a working EGL display to an app launched from the home screen, so
#     an SSH launch cannot use the GPU. Same handicap for every engine tested.
#   * --remote-debugging-port so the host can drive the page and read results.
# Everything it writes stays under /accounts/devuser/berrybench/data. It never
# touches the installed app, its profile, or the shared berry-kbd.log.
B=/accounts/devuser/berrybench
ENG="$1"; JSF="$2"; OZONE="$3"; URL="$4"
shift 4

cd "$B/data" || exit 1
export HOME="$B/data"
export LD_LIBRARY_PATH="$B"
export QNX_CA_BUNDLE="$B/root_store.certs"
export QNX_SCREEN_WIDTH=540 QNX_SCREEN_HEIGHT=540
export QNX_SCREEN_OUTPUT_WIDTH=1440 QNX_SCREEN_OUTPUT_HEIGHT=1440
export QNX_SCREEN_ROTATION=90
export QNX_INPUT_POLL_MS=1 QNX_MAX_FPS=45 QNX_GPU_PROBE=0
# The QNX port only starts the DevTools HTTP server when this is set.
export QNX_DEVTOOLS=1

JSARG=""
[ "$JSF" != "-" ] && JSARG="--js-flags=$JSF"

exec "$B/$ENG" \
  --no-sandbox --no-zygote --single-process \
  --disable-background-networking --disable-background-timer-throttling \
  --disable-renderer-backgrounding --disable-backgrounding-occluded-windows \
  --disable-client-side-phishing-detection --disable-default-apps \
  --disable-domain-reliability --disable-hang-monitor \
  --disable-prompt-on-repost --disable-sync --disable-translate \
  --disable-notifications --disable-smooth-scrolling --no-first-run \
  --no-default-browser-check --disable-component-update \
  --autoplay-policy=no-user-gesture-required \
  --disable-accelerated-video-decode --disable-logging \
  --ignore-certificate-errors --use-fake-ui-for-media-stream \
  --use-fake-device-for-media-stream --disable-quic \
  "--host-resolver-rules=MAP crashlogs.whatsapp.net ^NOTFOUND,MAP pixel.facebook.com ^NOTFOUND,MAP analytics.facebook.com ^NOTFOUND,MAP metric.facebook.com ^NOTFOUND" \
  --disable-renderer-accessibility \
  "--ozone-platform=$OZONE" \
  --disable-gpu --disable-gpu-compositing --num-raster-threads=4 \
  --force-device-scale-factor=1 \
  --disable-features=NetworkServiceDedicatedThread,MojoIpcz,Translate,OptimizationHints,MediaRouter,PreconnectToSearch,BackForwardCache,AutofillServerCommunication,HeavyAdPrivacyMitigations,InterestFeedContentSuggestions,FedCm,PrivacySandboxAdsAPIs,SharedStorageAPI,PrivateAggregationApi \
  --enable-low-end-device-mode \
  --enable-features=ImageDecodeConfigurableFeature:DecodedImageWorkingSetBudgetBytesForLowEndDevice/134217728 \
  --disk-cache-size=268435456 --media-cache-size=268435456 \
  --use-mobile-user-agent \
  ${JSARG:+"$JSARG"} \
  --blink-settings=hyperlinkAuditingEnabled=false,spellcheckEnabledByDefault=false \
  "--data-path=$B/data/profile" \
  --remote-debugging-port=9222 "--remote-allow-origins=*" \
  "$@" \
  "$URL"
