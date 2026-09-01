#!/bin/sh
# Stop the Berry Remote service (and the content_shell it drives).
killall content_shell 2>/dev/null
killall node.bin 2>/dev/null
echo "berry-remote stopped"
