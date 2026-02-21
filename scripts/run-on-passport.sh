#!/bin/bash
# Deploy and run content_shell on Passport.
# Usage: ./run-on-passport.sh [--deploy]

set -e
DEPLOY=0
URL="about:blank"

[[ "$1" == "--deploy" ]] && DEPLOY=1

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REMOTE_DIR="/accounts/devuser/berry-deploy"

echo "== Killing any stuck content_shell..."
ssh passport "slay content_shell 2>/dev/null" || true

if [[ $DEPLOY -eq 1 ]]; then
  echo "== Deploying..."
  ssh passport "rm -rf ${REMOTE_DIR}/* 2>/dev/null; mkdir -p ${REMOTE_DIR}"
  cd "$SCRIPT_DIR"
  scp -q content_shell content_shell.pak icudtl.dat ldqnx.so.2 libm.so.2 libgcc_s.so.1 libstdc++.so.6 \
      libtest_trace_processor.so libsafe_strlen.so shell_resources.pak \
      snapshot_blob.bin ui_resources_100_percent.pak passport:${REMOTE_DIR}/
  echo "== Deploy done."
fi

# content_shell uses --data-path. Use /accounts/1000/shared/documents to avoid LOCK quirks on devuser path.
DATA_BASE="/accounts/1000/shared/documents/berry-data"
echo "== Running content_shell..."
ssh passport "D=${DATA_BASE}_\$\$; rm -rf \${D} 2>/dev/null; mkdir -p \${D}; export LD_LIBRARY_PATH=${REMOTE_DIR}:\$LD_LIBRARY_PATH && cd ${REMOTE_DIR} && ./content_shell --no-sandbox --disable-gpu --no-zygote --single-process --headless --disable-features=ServiceWorker --data-path=\${D} --dump-dom \"${URL}\""
