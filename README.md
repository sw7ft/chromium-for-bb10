# Chromium for BlackBerry 10 (QNX ARM32)

A port of Chromium's `content_shell` to QNX, targeting the **BlackBerry Passport** (Snapdragon 801, ARMv7-A). This brings a modern (~2024-era) Chromium engine to a platform that was only ever shipped with an ancient WebKit fork.

## What Works

- **Headless browser** running in single-process mode on the BlackBerry Passport
- **HTML parsing and DOM construction** via Blink
- **V8 JavaScript engine** initialized and running on ARM32 QNX
- **`--dump-dom` output** for `about:blank`, `data:`, local HTTP, and **external HTTP** with clean exit
- **Local HTTP page loading** -- full HTML + JavaScript rendering via `http://127.0.0.1`
- **External HTTP page loading** -- fetching and rendering pages from the internet (e.g. `http://example.com`)
- **ICU internationalization**, CSS default stylesheets, full DOM tree

```
$ ./run.sh http://example.com 2>/dev/null
<html><head><title>Example Domain</title>...
<h1>Example Domain</h1>
<p>This domain is for use in documentation examples...</p>
...</html>
```

```
$ ./run.sh 'data:text/html,<h1>Hello from BB10</h1>' 2>/dev/null
<html><head></head><body><h1>Hello from BB10</h1></body></html>
```

## In Progress

- **Ozone platform for QNX Screen** -- windowed rendering backend using `screen_create_window()` + Skia software rasterizer
- **Browser chrome UI** -- Skia-rendered toolbar with URL bar, back/forward/reload buttons
- **BAR packaging** -- native app packaging for BB10 launcher (currently crashes on launch, needs Ozone debugging)

## What Doesn't Work (Yet)

- **HTTPS** -- TLS not yet tested
- **Multi-process mode** -- QNX process model differences
- **Service workers, web workers** -- disabled
- **Windowed mode** -- Ozone `qnx_screen` platform needs debugging

## Target Hardware

| | |
|---|---|
| **Device** | BlackBerry Passport (SQW100-1) |
| **SoC** | Qualcomm Snapdragon 801 |
| **CPU** | Quad-core Krait 400, ARMv7-A |
| **RAM** | 3 GB |
| **OS** | BlackBerry 10 (QNX 8.0.0) |

## Base Chromium Commit

```
ad76543128c308a9afdfc1ecf5b3f714886446c1
```

All patches apply against this commit. Fetch Chromium source using `depot_tools` and check out this commit before applying.

## Repository Structure

```
├── README.md
├── patches/
│   └── qnx-port.patch          # unified diff (~21k lines)
├── src/                         # 42 new QNX-specific files
│   ├── base/qnx/               # platform abstractions (memory, process, files, threading)
│   ├── build/config/qnx/       # GN platform config (defines, sysroot, flags)
│   ├── build/toolchain/qnx/    # GN toolchain (Clang 17 compile + GCC 9.3 link)
│   ├── content/                 # shell platform delegate, stubs, browser chrome
│   ├── gpu/qnx/                # GPU stubs
│   ├── sandbox/qnx/            # sandbox stubs
│   ├── ui/ozone/platform/qnx_screen/  # Ozone platform backend for QNX Screen
│   └── ...
├── deploy/
│   ├── README.md                # usage instructions for the binary
│   ├── run.sh                   # convenience launcher script
│   ├── package.sh               # build a deployable tarball
│   └── www/index.html           # sample test page
├── qnx_test_captures/           # verified milestone outputs (stdout + stderr)
├── build/
│   ├── args.gn                  # GN build arguments
│   └── ld-wrapper.sh            # linker wrapper (LLD + QNX libs)
├── scripts/
│   ├── apply-patches.sh         # apply everything to a Chromium checkout
│   ├── build.sh                 # build content_shell
│   └── run-on-passport.sh       # deploy + run on device
└── docs/
    ├── STATUS.md                # detailed status and issue log
    └── QNX_BB10_MILESTONES.md   # milestone descriptions and test captures
```

## Prerequisites

- **Linux host** (tested on Ubuntu in Docker)
- **Chromium source** checked out at commit `ad76543128` via `depot_tools`
- **QNX SDP 8.0** (or compatible) with ARM cross-compilation toolchain at `/root/qnx800/`
- **Clang 17** (`/usr/bin/clang-17`, `/usr/bin/clang++-17`)
- **LLD 17** (`/usr/bin/ld.lld-17`)
- **patchelf** for patching the ELF interpreter
- **SSH access** to a BlackBerry 10 device with developer mode enabled

## Build Instructions

### 1. Fetch Chromium and check out the base commit

```bash
mkdir ~/chromium && cd ~/chromium
fetch --nohooks chromium
cd src
git checkout ad76543128c308a9afdfc1ecf5b3f714886446c1
gclient sync -D
```

### 2. Apply the QNX port

```bash
cd /path/to/chromium-for-bb10
./scripts/apply-patches.sh ~/chromium/src
```

This applies `patches/qnx-port.patch` and copies the 42 new files into place.

### 3. Configure the build

```bash
cd ~/chromium/src
mkdir -p out/qnx-arm
cp /path/to/chromium-for-bb10/build/args.gn out/qnx-arm/args.gn
gn gen out/qnx-arm
```

### 4. Set up the linker wrapper

The QNX GCC toolchain's `ld` must be replaced with the LLD wrapper:

```bash
cp /path/to/chromium-for-bb10/build/ld-wrapper.sh \
   /root/qnx800/arm-blackberry-qnx8eabi/bin/ld
chmod +x /root/qnx800/arm-blackberry-qnx8eabi/bin/ld
```

### 5. Build

```bash
ninja -C out/qnx-arm content_shell
```

### 6. Deploy to device

```bash
# Patch the ELF interpreter for the device
patchelf --set-interpreter /accounts/devuser/berry-deploy/ldqnx.so.2 \
    out/qnx-arm/content_shell

# Copy binary + resources to device
scp out/qnx-arm/content_shell out/qnx-arm/content_shell.pak \
    out/qnx-arm/icudtl.dat out/qnx-arm/snapshot_blob.bin \
    out/qnx-arm/shell_resources.pak out/qnx-arm/ui_resources_100_percent.pak \
    passport:/accounts/devuser/berry-deploy/

# Also deploy QNX runtime libraries (ldqnx.so.2, libm.so.2, libgcc_s.so.1, libstdc++.so.6)
```

### 7. Package for deployment

```bash
# Build a self-contained tarball with the binary, runtime libs, and resources
./deploy/package.sh
# Creates deploy/bb10-content-shell.tar.gz (~60 MB)
```

### 8. Deploy and run

```bash
# Copy to device
scp -r deploy/bb10-content-shell/ passport:/accounts/devuser/bb10-content-shell/

# Run on device
ssh passport "cd /accounts/devuser/bb10-content-shell && ./run.sh http://example.com" 2>/dev/null

# Or with a data: URL
ssh passport "cd /accounts/devuser/bb10-content-shell && \
    ./run.sh 'data:text/html,<h1>Hello from BlackBerry</h1>'" 2>/dev/null

# Local HTTP (start python server first: python3.2 -m http.server 8001)
ssh passport "cd /accounts/devuser/bb10-content-shell && \
    ./run.sh http://127.0.0.1:8001/" 2>/dev/null
```

See [deploy/README.md](deploy/README.md) for detailed usage instructions.

## Build Configuration

```gn
target_os = "qnx"
target_cpu = "arm"
is_debug = false
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

## Toolchain

| Component | Tool | Path |
|-----------|------|------|
| C compiler | Clang 17 | `/usr/bin/clang-17` |
| C++ compiler | Clang 17 | `/usr/bin/clang++-17` |
| Linker | LLD 17 (via wrapper) | `/usr/bin/ld.lld-17` |
| Archiver | GCC 9.3 | `$QNX_HOST/bin/arm-blackberry-qnx8eabi-ar` |
| Sysroot | QNX SDP 8.0 | `/root/qnx800/arm-blackberry-qnx8eabi/` |

## Architecture Notes

The port works around several QNX-specific issues:

- **QNX ARM `std::string` bugs**: `find_first_of`, `find_first_not_of`, and `find(char, pos)` all return incorrect values (typically 0) on QNX ARM. Replaced with manual loop helpers across HTTP parsing, MIME type detection, chunked transfer decoding, and Mojo IPC deserialization.

- **Mojo IPC `HttpResponseHeaders` deserialization**: The `HttpResponseHeaders::Builder` API hangs on QNX due to the `std::string::find` bug. Bypassed by directly constructing from raw header strings.

- **URL path parsing bug**: QNX ARM codegen silently drops URL path components in `ParsePath()`. Bypassed with direct path assignment in `DoParseAfterScheme()`.

- **DOCTYPE crash**: Lowercase `<!doctype html>` causes SIGSEGV in `strlen` during HTML tokenization on QNX ARM. Fixed by normalizing to uppercase `<!DOCTYPE` in `HTMLDocumentParser::Append()`.

- **Clipboard hang**: `ui::Clipboard::IsSupportedClipboardBuffer()` hangs on QNX Ozone. Guarded with `#if !BUILDFLAG(IS_QNX)`.

- **TRACE_EVENT hangs**: Perfetto tracing macros cause hangs on QNX. All critical paths guarded with `#if !BUILDFLAG(IS_QNX)`.

- **libevent poll() broken**: `poll()` blocks indefinitely for external TCP sockets on QNX. Disabled poll backend, forced `select()` with minimum 50ms timeout.

- **Socket readiness detection**: QNX `select()` with zero timeout doesn't detect TCP socket readiness. Thread-pool-offloaded `select()` with retry mechanism (12x5s) used as workaround.

- **Mojo `EnableBatchDispatch`**: Disabled on QNX to prevent message delivery delays in `ThrottlingURLLoader`.

- **Content encoding**: Forced `Accept-Encoding: identity` on QNX to avoid gzip/deflate decompression issues.

- **Single-process only**: Due to QNX process model differences, only `--single-process` mode is supported.

- **Platform stubs**: Missing QNX APIs (sandboxing, GPU, UI platform delegate, etc.) are stubbed out.

See [docs/STATUS.md](docs/STATUS.md) for a detailed issue log.

## SSH Configuration

To connect to a BlackBerry Passport via USB:

```
Host passport
    HostName 169.254.0.1
    User devuser
    HostKeyAlgorithms +ssh-rsa
    PubkeyAcceptedAlgorithms +ssh-rsa
```

## License

The Chromium source code is licensed under the BSD license and other applicable licenses. See the [Chromium LICENSE](https://chromium.googlesource.com/chromium/src/+/HEAD/LICENSE) for details.
