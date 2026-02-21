#!/bin/bash
# Linker wrapper for QNX ARM cross-compilation.
# Place this at $QNX_TARGET/bin/ld so the GCC toolchain invokes it.
# Adjust paths to match your QNX SDK location.
exec /usr/bin/ld.lld-17 --error-limit=0 --allow-shlib-undefined --strip-debug --no-warn-mismatch --dynamic-linker=/usr/lib/ldqnx.so.2 -Map=/tmp/content_shell.map "$@" --no-pie /root/qnx800/arm-blackberry-qnx8eabi/lib/libatomic.a /root/qnx800/x86_64-linux/arm-blackberry-qnx8eabi/lib64/gcc/arm-blackberry-qnx8eabi/9.3.0/libgcc_eh.a
