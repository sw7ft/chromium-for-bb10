#!/bin/bash
# Package content_shell and all dependencies into a deployable tarball.
# Run from the chromium/src directory:
#   ./deploy/package.sh
#
# Produces: deploy/bb10-content-shell.tar.gz

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$SRC_DIR/out/qnx-arm"
QNX_TOOLCHAIN="/root/qnx800"

STAGING="$SCRIPT_DIR/bb10-content-shell"
rm -rf "$STAGING"
mkdir -p "$STAGING/www"

echo "Copying binary..."
cp "$BUILD_DIR/content_shell" "$STAGING/"

echo "Copying resource paks..."
cp "$BUILD_DIR/content_shell.pak" "$STAGING/"
cp "$BUILD_DIR/shell_resources.pak" "$STAGING/"
cp "$BUILD_DIR/ui_resources_100_percent.pak" "$STAGING/"

echo "Copying ICU data..."
cp "$BUILD_DIR/icudtl.dat" "$STAGING/"

echo "Copying V8 snapshot..."
cp "$BUILD_DIR/snapshot_blob.bin" "$STAGING/"

echo "Copying QNX runtime libraries..."
cp "$QNX_TOOLCHAIN/x86_64-linux/arm-blackberry-qnx8eabi/lib64/gcc/arm-blackberry-qnx8eabi/9.3.0/libgcc_s.so.1" "$STAGING/"
cp "$QNX_TOOLCHAIN/x86_64-linux/arm-blackberry-qnx8eabi/lib64/gcc/arm-blackberry-qnx8eabi/9.3.0/libstdc++.so.6" "$STAGING/"
cp "$QNX_TOOLCHAIN/arm-blackberry-qnx8eabi/lib/libm.so.2" "$STAGING/"

echo "Copying scripts and docs..."
cp "$SCRIPT_DIR/run.sh" "$STAGING/"
cp "$SCRIPT_DIR/README.md" "$STAGING/"
chmod +x "$STAGING/run.sh"

echo "Creating sample test page..."
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

echo "Creating tarball..."
cd "$SCRIPT_DIR"
tar czf bb10-content-shell.tar.gz bb10-content-shell/

SIZE=$(du -sh bb10-content-shell.tar.gz | cut -f1)
echo ""
echo "Done! Package created:"
echo "  $SCRIPT_DIR/bb10-content-shell.tar.gz ($SIZE)"
echo ""
echo "Deploy to device:"
echo "  tar xzf bb10-content-shell.tar.gz"
echo "  scp -r bb10-content-shell/ user@device:/accounts/devuser/"
