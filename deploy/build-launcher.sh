#!/bin/bash
# Cross-compile BerryShell launcher for QNX ARM and optionally deploy to Passport.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/berry-shell-bar/launcher.c"
OUT="$SCRIPT_DIR/berry-shell-bar/launcher"
QNX800="${QNX800:-/root/qnx800}"
GCC="$QNX800/bin/arm-blackberry-qnx8eabi-gcc"

if [ ! -f "$GCC" ]; then
  echo "Missing QNX cross compiler: $GCC" >&2
  exit 1
fi

export QNX_HOST="$QNX800/x86_64-linux"
export QNX_TARGET="$QNX800/arm-blackberry-qnx8eabi"
export QNX_INC="$QNX800/include"
export QNX_LIB="$QNX800/arm-blackberry-qnx8eabi/lib"

echo "=== compile launcher (QNX ARM) ==="
"$GCC" -o "$OUT" "$SRC"
file "$OUT"

APP_NATIVE="${PASSPORT_APP_NATIVE:-/accounts/1000/appdata/com.sw7ft.BerryShell.testDev__BerryShellf5089f54/app/native}"
if [ "${1:-}" = "--deploy" ]; then
  echo "=== deploy launcher -> $APP_NATIVE ==="
  scp "$OUT" "passport:$APP_NATIVE/launcher.new"
  ssh passport "chmod +x '$APP_NATIVE/launcher.new' && mv -f '$APP_NATIVE/launcher.new' '$APP_NATIVE/launcher' && echo launcher_ok"
fi
