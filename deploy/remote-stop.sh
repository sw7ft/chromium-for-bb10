#!/bin/sh
# Stop the Berry Remote service (and the content_shell it drives).
DIR="$(cd "$(dirname "$0")" && pwd)"
rm -f "$DIR/.remote-run"   # tells the supervisor loop to stop respawning
slay -f content_shell 2>/dev/null
slay -f node 2>/dev/null
echo "berry-remote stopped"
