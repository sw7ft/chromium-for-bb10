#!/bin/bash
# Full BB10 transfer bundle: content_shell headless + Node 22 + Berry Proxy.
# Run from chromium/src:
#   ./deploy/package-bundle.sh
#
# Produces: deploy/berry-browser-bundle-v1.zip

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$SRC_DIR/out/qnx-arm"
QNX_TOOLCHAIN="/root/qnx800"
NODE_SRC="${NODE_SRC:-/root/bb10-nodeJS-V22/deploy-term49/node}"
BUNDLE_VERSION="v1"

STAGING="$SCRIPT_DIR/berry-browser-bundle"
BUNDLE_NAME="berry-browser-bundle"
ZIP_OUT="$SCRIPT_DIR/berry-browser-bundle-${BUNDLE_VERSION}.zip"

rm -rf "$STAGING"
mkdir -p "$STAGING/www" "$STAGING/node"

echo "=== content_shell headless ==="
cp "$BUILD_DIR/content_shell" "$STAGING/"
if [ -f "$BUILD_DIR/libtest_trace_processor.so" ]; then
  echo "Copying libtest_trace_processor.so..."
  cp "$BUILD_DIR/libtest_trace_processor.so" "$STAGING/"
fi
cp "$BUILD_DIR/content_shell.pak" "$STAGING/"
cp "$BUILD_DIR/shell_resources.pak" "$STAGING/"
cp "$BUILD_DIR/ui_resources_100_percent.pak" "$STAGING/"
cp "$BUILD_DIR/icudtl.dat" "$STAGING/"
cp "$BUILD_DIR/snapshot_blob.bin" "$STAGING/"
cp "$QNX_TOOLCHAIN/x86_64-linux/arm-blackberry-qnx8eabi/lib64/gcc/arm-blackberry-qnx8eabi/9.3.0/libgcc_s.so.1" "$STAGING/"
cp "$QNX_TOOLCHAIN/x86_64-linux/arm-blackberry-qnx8eabi/lib64/gcc/arm-blackberry-qnx8eabi/9.3.0/libstdc++.so.6" "$STAGING/"
cp "$QNX_TOOLCHAIN/arm-blackberry-qnx8eabi/lib/libm.so.2" "$STAGING/"
cp "$SCRIPT_DIR/cacert.pem" "$STAGING/"

echo "=== scripts ==="
cp "$SCRIPT_DIR/run.sh" "$STAGING/"
cp "$SCRIPT_DIR/run-daemon.sh" "$STAGING/"
cp "$SCRIPT_DIR/kill-content-shell.sh" "$STAGING/"
cp "$SCRIPT_DIR/deploy-binary.sh" "$STAGING/"
cp "$SCRIPT_DIR/verify-passport.sh" "$STAGING/"
cp "$SCRIPT_DIR/test-regression.sh" "$STAGING/"
cp "$SCRIPT_DIR/test-daemon.sh" "$STAGING/"
cp "$SCRIPT_DIR/start-proxy.sh" "$STAGING/"
cp "$SCRIPT_DIR/proxy-server.js" "$STAGING/"
cp "$SCRIPT_DIR/README.md" "$STAGING/"
cp "$SCRIPT_DIR/HARDENING.md" "$STAGING/"
chmod +x "$STAGING/run.sh" "$STAGING/run-daemon.sh" "$STAGING/kill-content-shell.sh" "$STAGING/deploy-binary.sh" "$STAGING/verify-passport.sh" "$STAGING/test-regression.sh" "$STAGING/test-daemon.sh" "$STAGING/start-proxy.sh"

GIT_COMMIT=""
if git -C "$SRC_DIR" rev-parse --short HEAD >/dev/null 2>&1; then
  GIT_COMMIT="$(git -C "$SRC_DIR" rev-parse --short HEAD)"
fi
cat > "$STAGING/VERSION" <<VEOF
${BUNDLE_VERSION}
built=$(date -u +%Y-%m-%dT%H:%MZ)
commit=${GIT_COMMIT:-unknown}
VEOF

cat > "$STAGING/INSTALL.txt" <<INSTEOF
Berry Browser Bundle v1 for BlackBerry Passport (BB10 / QNX ARM32)
===================================================================

Release artifact: `berry-browser-bundle-v1.zip` (see also `VERSION` file).

Release: ${BUNDLE_VERSION}
See VERSION file for build date and commit.

Contents:
  content_shell   Chromium headless engine (~91 MB)
  node/           Node.js v22 (--jitless) for Berry Proxy
  proxy-server.js Localhost proxy for the stock BB10 browser
  run.sh          Headless DOM dump CLI
  run-daemon.sh   Persistent renderer (experimental; BERRY_USE_DAEMON=1)
  start-proxy.sh  Start proxy server

INSTALL ON DEVICE
-----------------
1. Copy this folder to the Passport, e.g.:
     /accounts/devuser/berry-deploy/

2. SSH in (or use Term49):
     cd /accounts/devuser/berry-deploy/berry-browser-bundle
     chmod +x run.sh run-daemon.sh start-proxy.sh test-regression.sh node/node

3. Test headless engine:
     ./run.sh https://example.com 2>/dev/null

4. Test Node:
     ./node/node -e "console.log(process.version)"

5. Start Berry Proxy:
     ./start-proxy.sh

6. Open in the BB10 stock browser:
     http://127.0.0.1:8765/

   Enter a URL (first load ~30-60 seconds).

REGRESSION
----------
  ./test-regression.sh 0

NOTES
-----
- Proxy renders pages in Chromium; stock browser shows HTML/CSS only.
- Berry Proxy uses one-shot mode by default (BERRY_USE_DAEMON=0).
- Keep entire folder together (LD_LIBRARY_PATH is relative).
- Free space needed: ~200 MB
INSTEOF

cat > "$STAGING/www/index.html" <<'HTMLEOF'
<!DOCTYPE html>
<html>
<head><title>Test Page</title></head>
<body>
  <h1>Local HTTP Works!</h1>
  <p>Served from BB10 Passport</p>
</body>
</html>
HTMLEOF

echo "=== Node.js 22 ==="
if [ ! -x "$NODE_SRC/node" ]; then
  echo "ERROR: Node bundle not found at $NODE_SRC" >&2
  echo "Set NODE_SRC to your BB10 node folder." >&2
  exit 1
fi
cp -a "$NODE_SRC/." "$STAGING/node/"
chmod +x "$STAGING/node/node" "$STAGING/node/node.bin" 2>/dev/null || true

echo "=== creating zip ==="
rm -f "$ZIP_OUT"
(cd "$SCRIPT_DIR" && zip -r -q "$ZIP_OUT" "$BUNDLE_NAME")

SIZE=$(du -sh "$ZIP_OUT" | cut -f1)
echo ""
echo "Done!"
echo "  $ZIP_OUT ($SIZE)"
echo ""
echo "Transfer to Passport:"
echo "  scp berry-browser-bundle-${BUNDLE_VERSION}.zip devuser@device:/accounts/devuser/"
echo "  unzip berry-browser-bundle-${BUNDLE_VERSION}.zip"
echo "  cd berry-browser-bundle && cat VERSION INSTALL.txt"
