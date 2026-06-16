#!/bin/sh
# Stop any stray content_shell processes (Passport thermal / orphaned runs).
killall content_shell 2>/dev/null
sleep 1
killall content_shell 2>/dev/null
