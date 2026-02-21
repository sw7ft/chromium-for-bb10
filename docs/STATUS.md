# QNX Content Shell - Status & Handover Document

**Last updated:** 2026-02-20
**Build status:** Issue 12 fixed. V8 Isolate init completes through `IH:4 InitDone` (LocalIsolate, HeapSetUp, Snapshot deserialization all done).

---

## Goal

Run Chromium `content_shell` on QNX (ARM32) on a BlackBerry Passport in headless single-process mode, successfully loading a URL.

---

## Deployment Strategy (Container → Passport)

**Context:** Build happens **on the container**. Deploy and run target **the Passport**. Keep any stuck `content_shell` processes killed on the Passport (`slay content_shell`) before deploy/run.

### berry-deploy contents (all in `/root/mytmp/berry-deploy/`)

All `.so` files and resources live in one directory. The binary is patched to use that directory for the ELF interpreter.

| File | Source | Purpose |
|------|--------|---------|
| `content_shell` | Build output, **patched** | Main binary |
| `ldqnx.so.2` | `/root/qnx800/arm-blackberry-qnx8eabi/usr/lib/` | ELF dynamic linker |
| `libm.so.2` | `/root/qnx800/arm-blackberry-qnx8eabi/lib/` | Math library |
| `libgcc_s.so.1` | GCC runtime | |
| `libstdc++.so.6` | C++ standard library | |
| `libtest_trace_processor.so` | Build | Trace processor |
| `libsafe_strlen.so` | Build | strlen override |
| `content_shell.pak`, `icudtl.dat`, `shell_resources.pak`, `snapshot_blob.bin`, `ui_resources_100_percent.pak` | Build | Resources |

### Patch binary (required after build)

The Passport may not have `/usr/lib/ldqnx.so.2`. Patch the binary to use the deploy-dir interpreter:

```bash
patchelf --set-interpreter /accounts/devuser/berry-deploy/ldqnx.so.2 /root/mytmp/berry-deploy/content_shell
```

### Deploy from container

Ensure all files (including `content_shell`) are in `/root/mytmp/berry-deploy/`. Then:

```bash
cd /root/mytmp/berry-deploy
ssh passport "slay content_shell 2>/dev/null; rm -rf /accounts/devuser/berry-deploy/*; mkdir -p /accounts/devuser/berry-deploy"
scp content_shell content_shell.pak icudtl.dat ldqnx.so.2 libm.so.2 libgcc_s.so.1 libstdc++.so.6 libtest_trace_processor.so libsafe_strlen.so shell_resources.pak snapshot_blob.bin ui_resources_100_percent.pak passport:/accounts/devuser/berry-deploy/
```

### Run on Passport

```bash
export LD_LIBRARY_PATH=/accounts/devuser/berry-deploy:$LD_LIBRARY_PATH
cd /accounts/devuser/berry-deploy
./content_shell --no-sandbox --disable-gpu --no-zygote --single-process --headless --dump-dom "data:text/html,<h1>Hello QNX</h1>"
```

Output goes straight to console (no sleep, no grep).

### One-command deploy + run (from container)

**Canonical workflow — use these exact commands after every rebuild:**

```bash
# 1. Build
cd /root/chromium/src && ninja -C out/qnx-arm content_shell

# 2. Copy to berry-deploy (required — run-on-passport.sh deploys from this dir)
cp /root/chromium/src/out/qnx-arm/content_shell /root/mytmp/berry-deploy/

# 3. Patch interpreter (required after every rebuild)
patchelf --set-interpreter /accounts/devuser/berry-deploy/ldqnx.so.2 /root/mytmp/berry-deploy/content_shell

# 4. Deploy and run on Passport
/root/mytmp/berry-deploy/run-on-passport.sh --deploy
```

Do not use subpaths like `berry-deploy/y/` — `run-on-passport.sh` expects files in `/root/mytmp/berry-deploy/` directly.

### Process: two systems, iterative debug

| Where | What |
|-------|------|
| **Container** (Linux dev env, `root@…`) | Build (`ninja`), patch (`patchelf`), deploy (`scp`), run script (`run-on-passport.sh` which SSHs to Passport) |
| **Passport** (QNX on device, `devuser@…` or `$`) | Runs `content_shell`; output shown in terminal. Must `exit` to return to container. |

**Before deploy:** Run `slay content_shell` on Passport if stuck (or script does it). Orphan processes slow the device.

**Debug cycle:** Add `write(2, "QNX:...\n", N)` traces → rebuild → patch → deploy → run → read last trace line before hang → add finer traces → repeat.

### ⚠️ Workflow: Deploy/run from local shell

**Deploy and run commands must be executed from the local shell.** After building, run the deploy commands manually and check the output. This avoids SSH/shell automation issues and provides direct control over when to deploy.

---

## Current State Summary

**We are 12 issues deep.** Issues 1–12 fixed.

- Issues 1–10: Fixed (deployment strategy, ELF interpreter, libm, V8 logger, etc.).
- **Issue 11:** ELF interpreter — binary expected `/usr/lib/ldqnx.so.2` on Passport; missing. **Fix:** Copy `ldqnx.so.2` to berry-deploy, patch binary with `patchelf`.
- **Issue 12 (FIXED):** V8 Isolate init hang in `LocalIsolate` → `default_locale_` → `icu::Locale` ctor. **Fix:** On QNX, bypass ICU in `Isolate::DefaultLocale()` and use `"en-US"` (`#if defined(__QNX__)`).

**Current state:** Issue 19 fixed (data: URLs). Issue 20 fixed: `DetermineOriginAgentClusterEndResult()` hang. Issue 21: `GetOriginToCommit()` hang — QNX bypass added in `GetOriginForURLLoaderFactoryAfterResponseWithDebugInfo()` for `WILL_COMMIT_WITHOUT_URL_LOADER` and for `READY_TO_COMMIT` + `about:blank`/`data:` URLs (state transitions to READY_TO_COMMIT before `GetOriginToCommit()` is called).

**Current hang (2026-02-21):** Run shows `NR_postMetrics` but still no `NR_postGetOrigin`, `NR_postGetOriginDebug`, `NR_preDelegate`, `NR_AfterDelegate`. Bypass may not be triggering (URL is `about:blank` per run-on-passport.sh). Renderer completes (`RTI:6 Done`) while browser appears stuck in or just after `GetOriginToCommit()`. **Paused** — next: verify bypass conditions or add more traces to narrow the hang.

---

## Development Notes

### Workflow: Deploy/run

Deploy and run commands (`run-on-passport.sh`, SSH, scp, or any remote execution) should be run manually from the local shell. Build (`ninja`) can be run directly in the container; patch and deploy/run are done from the host.

### Debug methodology (pick up from here)

1. **Build on container**, copy to berry-deploy, **patch** (required after every rebuild). Use these exact paths:
   ```bash
   cd /root/chromium/src && ninja -C out/qnx-arm content_shell
   cp /root/chromium/src/out/qnx-arm/content_shell /root/mytmp/berry-deploy/
   patchelf --set-interpreter /accounts/devuser/berry-deploy/ldqnx.so.2 /root/mytmp/berry-deploy/content_shell
   ```

2. **Deploy and run** — execute from local shell:
   ```bash
   /root/mytmp/berry-deploy/run-on-passport.sh --deploy
   ```
   (Patch step already done in step 1; script deploys from `/root/mytmp/berry-deploy/`.)

3. **Interpret traces:** Last QNX trace line before hang = hang location. Add finer traces in that function, rebuild, patch, deploy, run; repeat until the exact line is found.

4. **Current next step:** Browser reaches `NR_postMetrics` but never `NR_postGetOrigin`. Hang is in or just after `GetOriginToCommit()`. QNX bypass exists for READY_TO_COMMIT + about:blank/data:; either it's not triggering or hang is in `previous_render_frame_host->GetLastCommittedOrigin()` (same_origin_ comparison). Add trace at start of bypass to confirm; if bypass triggers, add trace after `GetOriginToCommit()` to see if hang is in `GetLastCommittedOrigin()`.

### Crash resolution
- Look for `QNX:CHROMIUM_CRASH sig=` for signal type
- Get `pc=0x<ADDR>` from crash line; resolve with:
  ```bash
  /root/qnx800/bin/arm-blackberry-qnx8eabi-addr2line -e /root/chromium/src/out/qnx-arm/exe.unstripped/content_shell -f -C 0x<ADDRESS>
  ```

### Key crash signals and their meaning
- **SIGTRAP (5):** `CHECK()` failure or `__builtin_trap()` — look for `Check failed:` in stderr
- **SIGSEGV (11):** Null pointer deref — look at `SEGV_MAPERR` address (0 = null deref)
- **SIGABRT (6):** `abort()` called — usually from `std::terminate` or assertion

---

## Issue 9: SIGSEGV in CSS Property Parsing — FIXED (pending test)

### Root Cause: Clang ARM32 Bitfield Layout Bug

**The bug:** Clang 17, when cross-compiling for ARM32 (`armv7-unknown-nto-qnx8.0.0eabi`), has an inconsistency between its constexpr evaluator and its code generator for `uint64_t` bitfields in classes with virtual functions:

- **Constexpr evaluator** (used for static initialization of `kCssProperties[]` array): Packs the `uint64_t` bitfield contiguously at **offset 4** after the 4-byte vtable pointer (no alignment padding).
- **Code generator** (used for `IsShorthand()`, `IsLonghand()`, etc.): Expects the bitfield at **offset 8** (with 4 bytes of padding for `uint64_t` 8-byte alignment).

This caused the `CSSProperty::IsShorthand()` function to read the wrong bits from memory. Longhand properties like `kColor` (property ID 2) were incorrectly identified as shorthands, leading to wrong virtual dispatch and a null pointer dereference.

### How it was diagnosed

1. Added raw memory dumps to `ParseValueStart` — showed the property_id was correctly stored at offset 4-5 (confirming bitfield at offset 4 in data).
2. Disassembled `ParseValueStart` — showed `ldr.w r8, [r4, #8]` (code reads bitfield from offset 8).
3. Wrote standalone test programs confirming both the data layout (contiguous at offset 4) and code access (split at offset 8) behaviors.
4. Cross-referenced object file hexdumps with runtime crash dumps to confirm the mismatch.

### The fix

Added explicit padding in `CSSProperty` (in `css_property.h`) between the vtable pointer and the bitfield, conditional on QNX ARM32:

```cpp
#if defined(__QNX__) && defined(__arm__)
  uint32_t qnx_arm_bf_pad_ = 0;  // Force bitfield to offset 8
#endif
  uint64_t property_id_ : 16;
  uint64_t repetition_separator_ : 8;
  uint64_t flags_ : 40;
```

This forces both the constexpr evaluator and code generator to place the bitfield at offset 8. The struct size remains 16 bytes (matching the existing `static_assert`).

### Affected struct: `CSSProperty` in `css_property.h`

Layout WITHOUT fix (QNX ARM32):
```
Offset 0: vtable pointer (4 bytes)
Offset 4: bitfield start (DATA view — constexpr init)
Offset 8: bitfield start (CODE view — runtime access)  ← MISMATCH!
```

Layout WITH fix:
```
Offset 0: vtable pointer (4 bytes)
Offset 4: explicit padding (4 bytes)
Offset 8: bitfield start (both DATA and CODE agree)
```

### Verified by disassembly

After the fix, the `kCssProperties[]` data in the binary shows the bitfield at offset 8 with proper padding at offset 4 (`00000000`), and the code still reads from offset 8 — they now agree.

---

## Chronological Issue Log

### Issue 1: Shared Memory / IPC — FIXED
- **Symptom**: Shared memory error during early init
- **Fix**: Environment variable / configuration changes for QNX shared memory

### Issue 2: SIGSEGV / Memory fault — FIXED
- **Symptom**: Segmentation fault during early init
- **Fix**: Platform-specific adjustments

### Issue 3: `mkstemp()` / `FilePath::Append()` — FIXED
- **Symptom**: Temp file creation failures
- **Fix**: QNX-specific temp directory handling

### Issue 4: `media::AudioManager` / `FakeAudioManager` — FIXED
- **Symptom**: Missing audio manager
- **Fix**: Stub/fake audio manager for QNX

### Issue 5: Aura / `ShellPlatformDelegate` — FIXED
- **Symptom**: Windowing system initialization failure in headless mode
- **Fix**: Platform delegate adjustments
- **Result**: **ENGINE STARTS AND STAYS RUNNING** — process alive, browser main loop running

### Issue 6: Hang in `ScopedCrashKeys` (NavigationRequest) — FIXED
- **Symptom**: Process hung at `NavigationRequest::Create()` → `ScopedCrashKeys crash_keys(*this)`
- **Fix**: Wrapped `ScopedCrashKeys` in `#if !BUILDFLAG(IS_QNX)` in:
  - `content/browser/renderer_host/navigation_request.cc`
  - `content/browser/renderer_host/render_frame_host_manager.cc`

### Issue 7: Hang in `GetFrameHostForNavigation` (SCOPED_CRASH_KEY macros) — FIXED
- **Symptom**: Hang in `SCOPED_CRASH_KEY_BOOL` / `SCOPED_CRASH_KEY_STRING64` macros
- **Fix**: Disabled `SCOPED_CRASH_KEY_*` macros for QNX in `render_frame_host_manager.cc`
- **Result**: Navigation flow fully progresses, renderer thread starts initializing

### Issue 8: SIGABRT from `std::bad_optional_access` — FIXED (was symptom of Issue 9)
- **Symptom**: `Abort (core dumped)` ~5-10 seconds into startup
- **Root cause**: Side-effect of the bitfield layout mismatch (Issue 9) causing corrupted flag reads, leading to bad virtual dispatch and eventually hitting an empty optional

### Issue 9: SIGSEGV in CSS Property Parsing (Clang ARM32 bitfield bug) — FIXED
- **Symptom**: `SEGV_MAPERR 000000000000` (null deref) during CSS font/color parsing
- **Crash site**: `ConsumeFontSize` / `ConsumeFontWeight` / `ConsumeColor` in Blink CSS
- **Root cause**: Clang ARM32 constexpr/codegen bitfield layout mismatch in `CSSProperty`
- **Fix**: Explicit padding in `css_property.h` (`uint32_t qnx_arm_bf_pad_`)

### Issue 11: SIGSEGV in GeneratedCodeCache / FileEnumerator — FIXED (pending test)
- **Symptom**: `SIGSEGV(11) SEGV_MAPERR 000000000000` during runtime; crash in `base::FilePath::FilePath(FilePath&&)` / `VectorBuffer<FilePath>::MoveRange` (file_enumerator_posix.cc)
- **Context**: Tasks from GeneratedCodeCache and AudioThreadHangMonitor; cache directory enumeration
- **Fix**: Disable GeneratedCodeCache for QNX in `ShellContentBrowserClient::GetGeneratedCodeCacheSettings()` — return `GeneratedCodeCacheSettings(false, 0, base::FilePath())`
- **File**: `content/shell/browser/shell_content_browser_client.cc`

### Issue 10: V8 Isolate init hang in V8FileLogger::SetUp — FIXED
- **Symptom**: Process hangs between `QNX:Init:0b9` (preLogger) and `QNX:Init:0b10` during V8 init
- **Hang sites** (traces pinpointed):
  1. **LogFile** temp-file creation — fixed by using `kLogToConsole` (stdout) on QNX
  2. **Ticker** constructor — `SamplingThread` creation hangs on QNX → skip Ticker on QNX; `ticker_` remains nullptr
- **Root cause**: `tmpfile()`/`FOpen()` and Ticker's `SamplingThread` hang on QNX
- **Fix**: `v8/src/logging/log.cc` — QNX uses stdout for LogFile; Ticker creation skipped (`--prof` disabled on QNX)
- **Status**: Binary rebuilt, deployed to `/root/mytmp/berry-deploy/`

---

## What's in the Binary Right Now

### Diagnostic overrides (`base/qnx_abort_override.cc`)
Two intercepts that fire on crash:
1. **`std::__throw_bad_optional_access()`** — strong override. Prints `QNX:BAD_OPTIONAL`, caller address (`QNX:OPT_C0`), and stack scan (`QNX:OS[N]`).
2. **`__wrap_abort()`** — via `--wrap=abort` linker flag. Prints `QNX:ABORT_WRAP` and caller.

### Trace instrumentation (all `write(2, "QNX:...\n", N)`)
| File | Traces | Purpose |
|------|--------|---------|
| `content/app/content_main_runner_impl.cc` | `QNX:I:10-12` | Main init flow |
| `content/renderer/in_process_renderer_thread.cc` | `QNX:Renderer:1-6` | Renderer thread init |
| `content/browser/renderer_host/navigation_controller_impl.cc` | `QNX:Nav:1-4`, `QNX:CNRFLP:1-6` | Navigation request setup |
| `content/browser/renderer_host/navigator.cc` | `QNX:NNav:1-4` | Navigation initiation |
| `content/browser/renderer_host/navigation_request.cc` | `QNX:NRCreate:1-2`, `QNX:NR:1-7` | Navigation request lifecycle |
| `content/browser/renderer_host/frame_tree_node.cc` | `QNX:FTN:1-4` | Frame tree node |
| `content/browser/renderer_host/render_frame_host_manager.cc` | `QNX:RFHM:1-2`, `QNX:GFHFN:1-6`, `QNX:Reinit:1-5`, `QNX:IRV:1-4` | Frame host management, ReinitializeMainRenderFrame, InitRenderView |
| `content/browser/web_contents/web_contents_impl.cc` | `QNX:CRV:1-5`, `QNX:CRWM:1-4` | CreateRenderViewForRenderManager, CreateRenderWidgetHostViewForRenderManager |
| `content/browser/web_contents/web_contents_view_aura.cc` | `QNX:CBWV:1-5` | CreateViewForWidget (preNew, pre/post InitAsChild, pre/post SetDragDropDelegate) |
| `third_party/blink/.../platform.cc` | `QNX:Blink:1-7` | Blink platform init |
| `third_party/blink/.../wtf.cc` | `QNX:WTF:1-6` | WTF library init |
| `third_party/blink/.../string_statics.cc` | `QNX:SS:1-7` | String statics init |
| `third_party/blink/.../atomic_string_table.cc` | `QNX:AST:1-5`, `QNX:ADD:1-3` | Atomic string table |
| `base/task/thread_pool/thread_group_impl.cc` | `QNX:FI:1-6` | Thread pool workers |
| `base/task/thread_pool/worker_thread.cc` | `QNX:W:0-11` | Worker thread lifecycle |
| `base/threading/platform_thread_posix.cc` | `QNX:TF:1-4` | Thread creation |
| `base/task/thread_pool/task_tracker.cc` | `QNX:TASK from=` | Task execution source (disabled with `&& 0`) |
| `content/renderer/render_frame_impl.cc` | `QNX:RFI:BindNavClient`, `QNX:RFI:CommitNav`, `QNX:RFI:CommitWithParams` | NavigationClient binding, CommitNavigation entry |
| `content/renderer/navigation_client.cc` | `QNX:NavClient:Commit` | Mojo CommitNavigation received (before RFI::CommitNavigation) |
| `content/browser/.../navigation_request.cc` | `NR_CommitNav`, `NR_CommitNav:preCoop`, `postCoop`, `preOrigin`, `postOrigin`, `beforeReadyToCommit`, `IsRenderFrameLive=0|1`, `NR_ReadyToCommit`, `NR_postMetrics`, `NR_preDelegate`, `NR_AfterDelegate` | Browser navigation flow; QNX single-process: 50ms yield before CommitNavigation |
| `content/browser/.../render_frame_host_impl.cc` | `QNX:Browser:RFHI_CommitNav`, `QNX:Browser:SendCommit` | Browser CommitNavigation and Mojo send |
| `blink/.../frame_loader.cc` | `QNX:FL:CommitNav` | FrameLoader::CommitNavigation |
| `blink/.../document_loader.cc` | `QNX:DL:CommitNav`, `QNX:DL:PreInstallDoc`, `QNX:DL:PostInstallDoc` | DocumentLoader commit and InstallNewDocument |
| `blink/.../html_document_parser.cc` | `QNX:PARSER:AppendBytes` | HTML parser receives body bytes |
| `blink/.../document.cc` | `QNX:DOC:CheckCompleted` | Document::CheckCompletedInternal |
| `base/debug/stack_trace_posix.cc` | `QNX:CHROMIUM_CRASH` | Chromium's signal handler |
| `gin/v8_initializer.cc` | `QNX:V8I:1-5` | V8 platform and snapshot init |
| `gin/isolate_holder.cc` | `QNX:IH:1-4` | IsolateHolder / Isolate creation |
| `platform/bindings/v8_per_isolate_data.cc` | `QNX:V8PID:1-3` | V8PerIsolateData ctor (postInitList, pre/post Enter) |
| `v8/src/snapshot/snapshot.cc` | `QNX:Snap:1-6` | Snapshot extraction and init |
| `v8/src/execution/isolate.cc` | `QNX:Init:0`–`0b10`, `0b10a`–`0b10f`, `0c`–`6` | Isolate::Init() phases (logger, stack guard, LocalIsolate, Unpark) |
| `v8/src/heap/local-heap.cc` | `QNX:LH:1`, `QNX:LH:2` | LocalHeap ctor (pre/post AddLocalHeap) |
| `v8/src/execution/local-isolate.cc` | `QNX:LI:2` | LocalIsolate ctor body (after all member init) |

### Diagnostic traces REMOVED in this build
The following CSS parsing debug traces were added during Issue 9 investigation and have been cleaned up:
- `css_property_parser.cc` — `QNX:PVS:` raw memory dump trace
- `css_parsing_utils.cc` — `QNX:PL:NULL_RANGE`, `QNX:CFS:NULL_RANGE`, `QNX:CFW:NULL_RANGE`
- `shorthands_custom.cc` — `QNX:CF:range=`, `QNX:FPS:range=`
- `style_builder.cc` — `QNX:NOT_LONGHAND` trace

### QNX-specific code changes (non-trace)
| File | Change |
|------|--------|
| `v8/src/logging/log.cc` | V8FileLogger: stdout + skip Ticker on QNX (avoid temp-file/hang) |
| `content/shell/browser/shell_content_browser_client.cc` | **NEW** — Disable GeneratedCodeCache on QNX (avoids FileEnumerator SIGSEGV) |
| `third_party/blink/.../css_property.h` | Explicit padding for ARM32 bitfield layout fix |
| `content/browser/renderer_host/navigation_request.cc` | `ScopedCrashKeys` disabled for QNX |
| `content/browser/renderer_host/render_frame_host_manager.cc` | `SCOPED_CRASH_KEY_*` macros disabled for QNX |
| `third_party/blink/.../platform/bindings/v8_per_isolate_data.cc` | Skip `SetAddCrashKeyCallback` on QNX (crash key infra hangs) |
| `content/browser/renderer_host/navigation_request.cc` | On QNX, NeedsUrlLoader() returns false for data: URLs (bypass network stack) |
| `content/browser/renderer_host/navigation_request.cc` | Skip RecordReadyToCommitMetrics on QNX (UMA/histogram infra blocks in single-process) |
| `content/browser/renderer_host/navigation_request.cc` | Skip MaybeRegisterOriginForUnpartitionedSessionStorageAccess and UpdatePrivateNetworkRequestPolicy on QNX (can block in single-process) |
| `content/browser/renderer_host/navigation_request.cc` | GetOriginForURLLoaderFactoryAfterResponseWithDebugInfo: QNX bypass returns opaque origin for WILL_COMMIT_WITHOUT_URL_LOADER or READY_TO_COMMIT+about:blank/data: |
| `content/app/content_main_runner_impl.cc` | Re-enabled `EnableInProcessStackDumping()` for QNX |
| `content/shell/BUILD.gn` | Added `ldflags = ["-Wl,--wrap=abort", "-Wl,--allow-multiple-definition"]` for QNX |
| `base/BUILD.gn` | Added `qnx_abort_override.cc` to sources for QNX |
| `third_party/blink/renderer/platform/wtf/casting.h` | `CHECK(IsA<Derived>(from))` → `DCHECK(...)` for QNX |

---

## Build & Deploy Reference

### Build args (`/root/chromium/src/out/qnx-arm/args.gn`)
```
target_os = "qnx"
target_cpu = "arm"
is_debug = false
dcheck_always_on = false
is_component_build = false
use_sysroot = false
treat_warnings_as_errors = false
clang_base_path = "/usr/lib/llvm-17"
clang_use_chrome_plugins = false
use_lld = false
enable_nacl = false
use_custom_libcxx = false
is_clang = true
use_glib = false
use_dbus = false
enable_rust = false
use_v8_context_snapshot = false
symbol_level = 1
```
**Note**: `symbol_level = 1` enables `addr2line` resolution. Set back to `0` for smaller production binaries later.

### Build commands
```bash
cd /root/chromium/src && ninja -C out/qnx-arm content_shell
```

### Linker wrapper (`/root/qnx800/arm-blackberry-qnx8eabi/bin/ld`)
```bash
#!/bin/bash
exec /usr/bin/ld.lld-17 --error-limit=0 --allow-shlib-undefined --strip-debug --no-warn-mismatch --dynamic-linker=/usr/lib/ldqnx.so.2 -Map=/tmp/content_shell.map "$@" --no-pie /root/qnx800/arm-blackberry-qnx8eabi/lib/libatomic.a /root/qnx800/x86_64-linux/arm-blackberry-qnx8eabi/lib64/gcc/arm-blackberry-qnx8eabi/9.3.0/libgcc_eh.a
```

### After rebuild: copy and patch
```bash
cp /root/chromium/src/out/qnx-arm/content_shell /root/mytmp/berry-deploy/
patchelf --set-interpreter /accounts/devuser/berry-deploy/ldqnx.so.2 /root/mytmp/berry-deploy/content_shell
/root/mytmp/berry-deploy/run-on-passport.sh --deploy
```
Always use `/root/mytmp/berry-deploy/` (not a subdir like `y/`). The script deploys from that directory.

### Resolve crash addresses (on build machine)
```bash
/root/qnx800/bin/arm-blackberry-qnx8eabi-addr2line -e /root/chromium/src/out/qnx-arm/exe.unstripped/content_shell -f -C 0x<ADDRESS>
```

### QNX cross-compiler tools (all in `/root/qnx800/bin/`)
- `arm-blackberry-qnx8eabi-gcc` / `g++` — compilers
- `arm-blackberry-qnx8eabi-objdump` — disassembly/symbols
- `arm-blackberry-qnx8eabi-addr2line` — address → file:line
- `arm-blackberry-qnx8eabi-nm` — symbol listing
- `arm-blackberry-qnx8eabi-c++filt` — C++ name demangling

---

## Device Environment Notes

- QNX device lacks `gzip`, `bash`, `file` — use `tar xf` (not `tar xzf`)
- Device shell is `ksh` (Korn shell), not bash
- Set `LD_LIBRARY_PATH` before running: `export LD_LIBRARY_PATH=/accounts/devuser/berry-deploy:$LD_LIBRARY_PATH`
- Shared memory needs writable `/tmp` — set `TMPDIR=/tmp` if issues
- The device has limited RAM (~2GB shared with display) — headless mode is essential
