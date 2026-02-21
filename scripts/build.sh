#!/bin/bash
# Build content_shell for QNX, copy to berry-deploy, and patch interpreter.
# Usage: ./build.sh
# Run from anywhere; script uses correct paths.

set -e
CHROMIUM_SRC="/root/chromium/src"
DEPLOY_DIR="/root/mytmp/berry-deploy"

echo "== Building content_shell..."
cd "$CHROMIUM_SRC"
ninja -C out/qnx-arm content_shell

echo "== Copying to berry-deploy..."
cp "$CHROMIUM_SRC/out/qnx-arm/content_shell" "$DEPLOY_DIR/"

echo "== Patching interpreter..."
patchelf --set-interpreter /accounts/devuser/berry-deploy/ldqnx.so.2 "$DEPLOY_DIR/content_shell"

echo "== Build done. Deploy with: $DEPLOY_DIR/run-on-passport.sh --deploy"
