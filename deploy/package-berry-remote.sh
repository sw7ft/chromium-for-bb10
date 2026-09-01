#!/bin/bash
# Berry Remote — self-contained package for BerryCore / external work.
#
# Produces deploy/berry-remote-build<N>.zip containing EVERYTHING needed on a
# BB10 device (no toolchain required):
#   berry-browser-bundle/   engine (content_shell + paks + ICU + snapshot +
#                           certs + runtime libs) + berry-remote.js +
#                           berry-viewer.html + remote-start.sh/remote-stop.sh
#   node/                   QNX ARM32 Node runtime (node + libuv/libstdc++/
#                           libgcc). MUST be run with --jitless (the start
#                           script does this): the JIT emits broken code on
#                           this port and SIGBUSes once anything tiers up.
#   README.md               architecture, install, usage, endpoints
#
# Run from chromium/src:  ./deploy/package-berry-remote.sh
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$SRC_DIR/out/qnx-arm"
PAYLOAD="$SCRIPT_DIR/berry-shell-bar/payload"
NODE_TGZ="/root/node-qnx/node-qnx-arm32.tar.gz"
STAGING="/tmp/berry-remote-pkg"
BUILD_NUM="$(sed -n 's:.*<buildId>\([^<]*\)</buildId>.*:\1:p' \
  "$SCRIPT_DIR/berry-shell-bar/bar-descriptor-v3.xml" | head -1)"
BUILD_NUM="${BUILD_NUM:-0}"
ZIP_OUT="$SCRIPT_DIR/berry-remote-build${BUILD_NUM}.zip"

echo "=== ninja content_shell ==="
ninja -C "$BUILD_DIR" content/shell:content_shell

echo "=== stage ==="
rm -rf "$STAGING"
mkdir -p "$STAGING/berry-browser-bundle" "$STAGING/node"
B="$STAGING/berry-browser-bundle"

# Engine + resources (loaded from the dir containing the executable).
cp "$BUILD_DIR/content_shell" "$B/"
cp "$BUILD_DIR/content_shell.pak" "$B/"
cp "$BUILD_DIR/shell_resources.pak" "$B/"
cp "$BUILD_DIR/ui_resources_100_percent.pak" "$B/"
cp "$BUILD_DIR/icudtl.dat" "$B/"
cp "$BUILD_DIR/snapshot_blob.bin" "$B/"
cp "$PAYLOAD/root_store.certs" "$B/"
cp "$PAYLOAD/libgcc_s.so.1" "$PAYLOAD/libstdc++.so.6" "$PAYLOAD/libm.so.2" "$B/"
[ -f "$PAYLOAD/libasound.so.2" ] && cp "$PAYLOAD/libasound.so.2" "$B/"

# Service + viewer + scripts.
cp "$SCRIPT_DIR/berry-remote.js" "$SCRIPT_DIR/berry-viewer.html" \
   "$SCRIPT_DIR/remote-start.sh" "$SCRIPT_DIR/remote-stop.sh" "$B/"

# Node runtime (extract locally; devices lack gzip).
tar xzf "$NODE_TGZ" -C "$STAGING/node"
rm -f "$STAGING/node/deploy.sh"

GIT_COMMIT="$(git -C "$SRC_DIR" rev-parse --short HEAD 2>/dev/null || echo unknown)"

cp "$SCRIPT_DIR/BERRY-REMOTE.md" "$STAGING/README.md"
cat >> "$STAGING/README.md" <<EOF

## Package install (self-contained)

Copy both directories to the device side by side, e.g.:

\`\`\`
/accounts/devuser/berry-deploy/
├── berry-browser-bundle/   (chmod +x content_shell *.sh)
└── node/                   (chmod +x node)
\`\`\`

Then:

\`\`\`sh
cd .../berry-browser-bundle
chmod +x content_shell node ../node/node *.sh 2>/dev/null
PORT=8080 URL='https://www.google.com/' sh remote-start.sh
sh remote-stop.sh
\`\`\`

Notes for porters:
- Binaries only execute inside an app perimeter (Term49/BerryCore sandbox) or
  from /accounts/devuser over SSH. BB10 denies exec from shared/ and /tmp.
- remote-start.sh runs node with --jitless (required; see header comment) and
  supervises the service: if the engine hits a fatal CHECK and dies, it
  respawns in ~3s. The .remote-run marker file holds the supervisor nonce.
- The device busybox has no killall/nohup/setsid/gzip/head; use slay, trap ''
  HUP, and pre-extracted files.

## Build info

- build: ${BUILD_NUM}
- date: $(date -u +%Y-%m-%dT%H:%MZ)
- commit: ${GIT_COMMIT}
EOF

cat > "$STAGING/VERSION" <<EOF
build=${BUILD_NUM}
built=$(date -u +%Y-%m-%dT%H:%MZ)
commit=${GIT_COMMIT}
package=berry-remote
EOF

echo "=== zip ==="
rm -f "$ZIP_OUT"
( cd "$STAGING" && zip -qr "$ZIP_OUT" . )

echo ""
echo "Done: $ZIP_OUT"
ls -lh "$ZIP_OUT"
