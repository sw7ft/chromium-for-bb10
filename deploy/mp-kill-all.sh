#!/bin/sh
# Stop all BerryShell / content_shell processes on Passport.
# Multi-process failures often leave utility+renderer spinning after the browser dies.
# Run this (or mp-test-safe's trap) until hogs is clean.

killall content_shell 2>/dev/null
killall content_shell.exe 2>/dev/null
slay -9 content_shell 2>/dev/null
slay -9 content_shell.exe 2>/dev/null
slay -9 launcher 2>/dev/null
sleep 2
killall content_shell 2>/dev/null
killall content_shell.exe 2>/dev/null
slay -9 content_shell 2>/dev/null
slay -9 content_shell.exe 2>/dev/null
slay -9 launcher 2>/dev/null
sleep 2
killall content_shell 2>/dev/null
killall content_shell.exe 2>/dev/null
slay -9 content_shell 2>/dev/null
slay -9 content_shell.exe 2>/dev/null
