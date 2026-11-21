# ThinLTO build (production since build 95)

The port was already ThinLTO-ready without knowing it: every compile is
clang-17 (`is_clang=true`) and the toolchain's `ld` at
`/root/qnx800/arm-blackberry-qnx8eabi/bin/ld` has been a shim exec'ing
`ld.lld-17` (with `--no-pie`, `--dynamic-linker=/usr/lib/ldqnx.so.2`,
`libatomic.a`, `libgcc_eh.a`, `--strip-debug`) since Feb 2026. lld performs
ThinLTO natively when handed bitcode objects and ignores the GCC driver's
`-plugin` args.

## Output dir

`out/qnx-arm-lto` = `out/qnx-arm/args.gn` plus:

```
use_lld = true        # gates GN LTO config; -fuse-ld itself suppressed on QNX
use_thin_lto = true
```

Build: `ninja -C out/qnx-arm-lto content_shell`
Package: `BERRY_CONTENT_SHELL=$PWD/out/qnx-arm-lto/content_shell deploy/build-v3-bar.sh`

Full build ~2.5h on 24 cores; ThinLTO link ~9 min cold, ~2 min warm
(`out/qnx-arm-lto/thinlto-cache`). Binary ~86MB stripped vs 96MB non-LTO.

## Link-step optimization (`--lto-O2`, since build 110)

Up to build 109 every target linked with `-Wl,--lto-O0`: ThinLTO did its
cross-module import, but the optimization passes it defers to the link were
skipped. `thin_lto_enable_optimizations` defaults to false on QNX, and
`content_shell` was not opted in to `thinlto_optimize_max` the way `chrome`
and `headless_shell` are.

`out/qnx-arm-pgo-o2` = `out/qnx-arm-pgo/args.gn` plus:

```
thin_lto_enable_optimizations = true
```

and `content/shell/BUILD.gn` swaps `thinlto_optimize_default` for
`thinlto_optimize_max` on the `content_shell` executable. Without the arg the
swap resolves to the same `--lto-O0` as before, so `out/qnx-arm-pgo` is
unaffected and remains the fallback.

Package: `BERRY_CONTENT_SHELL=$PWD/out/qnx-arm-pgo-o2/content_shell deploy/build-v3-bar.sh`

- Binary 90.4 MB stripped vs 83.1 MB at O0.
- One new libc import, `__strncpy_chk`, which the BB10 libc exports.
- Measured on a Passport with `deploy/berrybench` (ten DOM-heavy Speedometer
  3.0 suites, software rendering, 12 iterations each, interleaved): 17,959 ms
  per iteration vs 19,566 ms at O0, 8% less.

## QNX-specific fixes (all committed)

1. `build/config/compiler/BUILD.gn`: the link driver is BB10 g++ 9.3, so the
   clang-only driver flags are gated off for QNX: `-fuse-ld=lld`,
   `-flto=thin` (link), `-fwhole-program-vtables` (link; replaced by
   `-Wl,--lto-whole-program-visibility`). All `-Wl,*` LTO flags pass through.
2. **Emulated TLS** (`-Wl,-mllvm,-emulated-tls`): the port compiles with
   `-femulated-tls` (QNX defines no `__aeabi_read_tp`), but clang-17 doesn't
   record that in bitcode, so the LTO backend regenerated every
   `thread_local` access against the generic ELF model -> 835 undefined
   `__aeabi_read_tp` refs at link.
3. `platform_thread_posix.cc`: `QnxThreadFuncAligned` is referenced only from
   inline asm in the naked realign trampoline; LTO can't see asm refs and
   internalized it away -> `__attribute__((used, retain))`.
4. `qnx_platform_stubs.cc`: **stack-protector guard** (the build-94 boot
   crash). LLVM's ARM backend lowers canary access under LTO to
   movw/movt + double load (slot -> pointer -> canary), but lld resolved
   libc's `__stack_chk_guard` as R_ARM_COPY, so the slot held the canary
   value and the second load dereferenced canary bytes: SIGSEGV at
   `strlen+0xd` (our interposed strlen is the first protected function the
   loader calls), fault address different every boot. Fix: the exe defines
   `__stack_chk_guard` as a POINTER to a local canary, which makes the
   indirect pattern correct and keeps GOT-style consumers self-consistent.

## Verification notes

- 34 R_ARM_COPY relocs remain (stdio, std::cout, libstdc++ vtables, environ,
  _syspage_ptr) — standard non-PIE single-load semantics, loader-handled.
- Build 95 verified on device: boots, X sessions stable, stalls remain
  synchronous-JS (never GC), worst-case load stalls improved vs 93 baseline.
