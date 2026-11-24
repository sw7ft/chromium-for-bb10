#!/bin/bash
# Upload a freshly built engine to the phone's bench directory as new/cs-new.
# Never touches the installed app.
#   push-new.sh [path/to/content_shell]   (default: out/qnx-arm-pgo-o2)
set -e
SRC="${1:-$(cd "$(dirname "$0")/../.." && pwd)/out/qnx-arm-pgo-o2/content_shell}"
B=/accounts/devuser/berrybench
PHONE=passport-black-lan
ssh -o BatchMode=yes $PHONE "slay -f cs-new 2>/dev/null; true"
scp -q "$SRC" "$PHONE:$B/cs-new.upload"
scp -q "$(dirname "$SRC")/snapshot_blob.bin" "$PHONE:$B/snap-new.upload"
ssh -o BatchMode=yes $PHONE "cd $B && on -u devuser /bin/sh -c 'mkdir -p new && cd new && \
  for f in content_shell.pak home.html icudtl.dat root_store.certs shell_resources.pak ui_resources_100_percent.pak; do ln -sf ../\$f \$f; done; \
  cp ../snap-new.upload snapshot_blob.bin && cp ../cs-new.upload cs-new && chmod 755 cs-new'; rm -f cs-new.upload snap-new.upload; ls -la new/cs-new"
