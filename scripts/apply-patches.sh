#!/bin/bash
# Apply QNX port patches and new files to a Chromium source checkout.
# Usage: ./apply-patches.sh /path/to/chromium/src

set -e

if [ -z "$1" ]; then
  echo "Usage: $0 /path/to/chromium/src"
  exit 1
fi

CHROMIUM_SRC="$(cd "$1" && pwd)"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ ! -f "$CHROMIUM_SRC/BUILD.gn" ]; then
  echo "Error: $CHROMIUM_SRC does not look like a Chromium source directory"
  exit 1
fi

echo "Chromium source: $CHROMIUM_SRC"
echo "QNX port repo:   $SCRIPT_DIR"
echo

echo "== Applying patch (324 modified files)..."
cd "$CHROMIUM_SRC"
git apply --stat "$SCRIPT_DIR/patches/qnx-port.patch"
git apply "$SCRIPT_DIR/patches/qnx-port.patch"
echo "   Patch applied."
echo

echo "== Copying new QNX files (21 files)..."
cp -r "$SCRIPT_DIR/src/"* "$CHROMIUM_SRC/"
echo "   Files copied."
echo

echo "== Verifying..."
NEW_COUNT=$(find "$SCRIPT_DIR/src" -type f | wc -l)
echo "   $NEW_COUNT new files placed into source tree."

echo
echo "Done. Next steps:"
echo "  1. mkdir -p out/qnx-arm"
echo "  2. cp $SCRIPT_DIR/build/args.gn out/qnx-arm/args.gn"
echo "  3. gn gen out/qnx-arm"
echo "  4. ninja -C out/qnx-arm content_shell"
