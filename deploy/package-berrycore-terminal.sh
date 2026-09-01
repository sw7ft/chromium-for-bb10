#!/bin/bash
# BerryCore terminal browser package — full on-screen content_shell for SSH/Term49.
#
# Produces a zip you can hand to the BerryCore agent:
#   deploy/browser-chromium-terminal-<build>.zip
#
# Layout (BerryCore standard):
#   bin/berry-browser      small launcher (same flags as BerryShell .bar)
#   bin/content_shell      Chromium engine + colocated resource files
#   lib/                   bundled runtime libs
#   share/berry-browser/   optional home.html
#   doc/README.md
#
# Run from chromium/src:
#   ./deploy/package-berrycore-terminal.sh
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$SRC_DIR/out/qnx-arm"
QNX800="${QNX800:-/root/qnx800}"
GCC="$QNX800/bin/arm-blackberry-qnx8eabi-gcc"
export QNX_HOST="$QNX800/x86_64-linux"
export QNX_TARGET="$QNX800/arm-blackberry-qnx8eabi"
export QNX_INC="$QNX800/include"
export QNX_LIB="$QNX800/arm-blackberry-qnx8eabi/lib"
PAYLOAD="$SCRIPT_DIR/berry-shell-bar/payload"
STAGING="/tmp/berrycore-browser-pkg"
BUILD_NUM="$(sed -n 's:.*<buildId>\([^<]*\)</buildId>.*:\1:p' \
  "$SCRIPT_DIR/berry-shell-bar/bar-descriptor-v3.xml" | head -1)"
BUILD_NUM="${BUILD_NUM:-0}"
ZIP_OUT="$SCRIPT_DIR/browser-chromium-terminal-build${BUILD_NUM}.zip"

echo "=== ninja content_shell ==="
ninja -C "$BUILD_DIR" content/shell:content_shell

echo "=== stage BerryCore package ==="
rm -rf "$STAGING"
mkdir -p "$STAGING/bin" "$STAGING/lib" "$STAGING/share/berry-browser" \
  "$STAGING/doc"

# QNX loads paks/ICU/snapshot from the directory containing the executable
# (CHROME_EXE_PATH). Keep them beside content_shell in bin/.
cp "$BUILD_DIR/content_shell" "$STAGING/bin/"
cp "$BUILD_DIR/content_shell.pak" "$STAGING/bin/"
cp "$BUILD_DIR/shell_resources.pak" "$STAGING/bin/"
cp "$BUILD_DIR/ui_resources_100_percent.pak" "$STAGING/bin/"
cp "$BUILD_DIR/icudtl.dat" "$STAGING/bin/"
cp "$BUILD_DIR/snapshot_blob.bin" "$STAGING/bin/"
cp "$PAYLOAD/root_store.certs" "$STAGING/bin/"

cp "$PAYLOAD/libgcc_s.so.1" "$STAGING/lib/"
cp "$PAYLOAD/libstdc++.so.6" "$STAGING/lib/"
cp "$PAYLOAD/libm.so.2" "$STAGING/lib/"
if [ -f "$PAYLOAD/libasound.so.2" ]; then
  cp "$PAYLOAD/libasound.so.2" "$STAGING/lib/"
fi

# Terminal launcher: BerryShell launcher.c execs content_shell (not .exe).
LAUNCHER_SRC="$SCRIPT_DIR/berry-shell-bar/launcher.c"
LAUNCHER_TMP="$STAGING/launcher-terminal.c"
sed 's/content_shell\.exe/content_shell/g' "$LAUNCHER_SRC" > "$LAUNCHER_TMP"
sed -i 's/BerryShell:/BerryBrowser:/g' "$LAUNCHER_TMP"
"$GCC" -O2 -o "$STAGING/bin/berry-browser" "$LAUNCHER_TMP"
rm -f "$LAUNCHER_TMP"
chmod +x "$STAGING/bin/content_shell" "$STAGING/bin/berry-browser"

if [ -f "$SCRIPT_DIR/berry-shell-bar/home.html" ]; then
  cp "$SCRIPT_DIR/berry-shell-bar/home.html" "$STAGING/share/berry-browser/"
fi

GIT_COMMIT=""
if git -C "$SRC_DIR" rev-parse --short HEAD >/dev/null 2>&1; then
  GIT_COMMIT="$(git -C "$SRC_DIR" rev-parse --short HEAD)"
fi

cat > "$STAGING/doc/README.md" <<EOF
# BerryBrowser terminal (BerryCore)

Full on-screen Chromium browser for BB10 — run from SSH, Term49, or any
terminal. Uses the \`qnx_screen\` ozone backend (touch + hardware keyboard).

## Install

Extract into your BerryCore ports tree, e.g.:

\`\`\`
browser-chromium-terminal/
├── bin/
├── lib/
├── share/
└── doc/
\`\`\`

## Usage

\`\`\`bash
# Default start page (bundled home or berry-home-url marker)
berry-browser

# Open a URL
berry-browser https://www.google.com
berry-browser https://berry.settings/

# Stop
killall content_shell
\`\`\`

## Settings

Same marker files as the BerryShell .bar app (in \`/accounts/1000/shared/misc/\`):

- \`berry-device\` — passport, q10, z10, etc. (touch/viewport)
- \`berry-x-720.enable\` / \`berry-x-420.enable\` — render resolution tier
- \`berry-gpu.disable\` — force software rendering
- \`berry-desktop.enable\` — desktop UA

After changing markers, relaunch \`berry-browser\`.

## Requires on device

- QNX \`libscreen\`, \`libbps\` (system)
- OpenAL (\`libOpenAL.so.1\`) — usually from BerryCore \`openal\` port
- Optional: BerryCore CA at \`/accounts/1000/shared/misc/berrycore/ssl/cert.pem\`
  (bundled \`root_store.certs\` is used by default)

## Build info

- build: ${BUILD_NUM}
- date: $(date -u +%Y-%m-%dT%H:%MZ)
- commit: ${GIT_COMMIT:-unknown}
- binary: $(file -b "$STAGING/bin/content_shell" | head -1)
EOF

cat > "$STAGING/VERSION" <<EOF
build=${BUILD_NUM}
built=$(date -u +%Y-%m-%dT%H:%MZ)
commit=${GIT_COMMIT:-unknown}
package=browser-chromium-terminal
EOF

echo "=== zip ==="
rm -f "$ZIP_OUT"
( cd "$STAGING" && zip -r "$ZIP_OUT" . )

echo ""
echo "Done: $ZIP_OUT"
ls -lh "$ZIP_OUT" "$STAGING/bin/content_shell"
echo ""
echo "BerryCore INDEX suggestion:"
echo "browser-chromium-terminal|net|${BUILD_NUM}|$(du -h "$ZIP_OUT" | cut -f1)|Chromium on-screen browser (terminal/SSH)"
