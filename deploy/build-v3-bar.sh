#!/bin/bash
# Build BerryBrowserV3 .bar for BlackBerry 10 (QNX ARM32).
#
# Output filename is derived from bar-descriptor-v3.xml:
#   BerryBrowserV3-<version>-build<buildId>.bar
# e.g. BerryBrowserV3-3.0.1-build2.bar
#
# Run from chromium/src:  ./deploy/build-v3-bar.sh
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BAR_DIR="$SCRIPT_DIR/berry-shell-bar"
DESC="$BAR_DIR/bar-descriptor-v3.xml"
BBNDK_ENV="${BBNDK_ENV:-/root/bbndk/bbndk-env_10_3_1_995.sh}"

QNX800="${QNX800:-/root/qnx800}"
GCC="$QNX800/bin/arm-blackberry-qnx8eabi-gcc"
export QNX_HOST="$QNX800/x86_64-linux"
export QNX_TARGET="$QNX800/arm-blackberry-qnx8eabi"
export QNX_INC="$QNX800/include"
export QNX_LIB="$QNX800/arm-blackberry-qnx8eabi/lib"

CHROMIUM_OUT="$(cd "$SCRIPT_DIR/.." && pwd)/out/qnx-arm/content_shell"
if [ ! -f "$CHROMIUM_OUT" ]; then
  echo "Missing $CHROMIUM_OUT — run ninja -C out/qnx-arm content/shell:content_shell first." >&2
  exit 1
fi
echo "=== sync payload/content_shell from out/qnx-arm ==="
cp -f "$CHROMIUM_OUT" "$BAR_DIR/payload/content_shell"

VERSION="$(sed -n 's:.*<versionNumber>\([^<]*\)</versionNumber>.*:\1:p' "$DESC" | head -1)"
BUILD="$(sed -n 's:.*<buildId>\([^<]*\)</buildId>.*:\1:p' "$DESC" | head -1)"
if [ -z "$VERSION" ] || [ -z "$BUILD" ]; then
  echo "Could not read versionNumber/buildId from $DESC" >&2
  exit 1
fi

BAR_NAME="BerryBrowserV3-${VERSION}-build${BUILD}.bar"
BAR_OUT="$BAR_DIR/$BAR_NAME"

echo "=== compile launcher (QNX ARM) ==="
"$GCC" -O2 -o "$BAR_DIR/launcher" "$BAR_DIR/launcher.c"
file "$BAR_DIR/launcher"

echo "=== package $BAR_OUT ==="
source "$BBNDK_ENV" >/dev/null 2>&1 || true
( cd "$BAR_DIR" && blackberry-nativepackager -package "$BAR_NAME" "$DESC" )

ls -la "$BAR_OUT"
echo "Done: $BAR_OUT"

if [ "${BERRY_SMOKE:-}" = "1" ] || [ "${1:-}" = "--smoke" ]; then
  echo ""
  echo "=== YouTube shim smoke test ==="
  echo "Install $BAR_OUT on Passport, force-kill BerryBrowserV3,"
  echo "open home -> YouTube tile, wait ~30s for playback, then press Enter."
  read -r
  "$SCRIPT_DIR/smoke-youtube-shim.sh"
fi
