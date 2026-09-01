#!/bin/bash
# Build the standalone WhatsApp web app .bar for BlackBerry 10 (QNX ARM32).
#
#   - cross-compiles whatsapp-launcher.c (the single-purpose boot wrapper)
#   - packages BerryWhatsApp.bar from bar-descriptor-whatsapp.xml
#
# Reuses the SAME engine payload as BerryBrowserV3 (deploy/berry-shell-bar/
# payload/), so no content_shell rebuild is required -- the desktop-UA flip for
# *.whatsapp.com and the persistent per-app profile are already in the engine.
#
# Run from chromium/src:  ./deploy/build-whatsapp-bar.sh
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BAR_DIR="$SCRIPT_DIR/berry-shell-bar"
SRC="$BAR_DIR/whatsapp-launcher.c"
OUT="$BAR_DIR/whatsapp-launcher"
DESC="$BAR_DIR/bar-descriptor-whatsapp.xml"
BAR_OUT="$BAR_DIR/BerryWhatsApp.bar"

QNX800="${QNX800:-/root/qnx800}"
GCC="$QNX800/bin/arm-blackberry-qnx8eabi-gcc"
BBNDK_ENV="${BBNDK_ENV:-/root/bbndk/bbndk-env_10_3_1_995.sh}"

if [ ! -f "$GCC" ]; then
  echo "Missing QNX cross compiler: $GCC" >&2
  exit 1
fi
if [ ! -f "$BAR_DIR/payload/content_shell" ]; then
  echo "Missing engine payload: $BAR_DIR/payload/content_shell" >&2
  echo "Populate berry-shell-bar/payload/ (same as BerryBrowserV3) first." >&2
  exit 1
fi

export QNX_HOST="$QNX800/x86_64-linux"
export QNX_TARGET="$QNX800/arm-blackberry-qnx8eabi"
export QNX_INC="$QNX800/include"
export QNX_LIB="$QNX800/arm-blackberry-qnx8eabi/lib"

echo "=== compile whatsapp-launcher (QNX ARM) ==="
"$GCC" -O2 -o "$OUT" "$SRC"
file "$OUT"

echo "=== package $BAR_OUT ==="
# blackberry-nativepackager is a Java app; source the NDK env so its bundled JRE
# is on PATH. Run from BAR_DIR so the descriptor's relative asset paths resolve.
# shellcheck disable=SC1090
source "$BBNDK_ENV" >/dev/null 2>&1 || true
( cd "$BAR_DIR" && blackberry-nativepackager -package "$BAR_OUT" "$DESC" )

ls -la "$BAR_OUT"
echo "Done: $BAR_OUT"
