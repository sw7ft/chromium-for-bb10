# Chromium for BlackBerry 10 (QNX ARM32)

A port of Chromium's `content_shell` to QNX, targeting the **BlackBerry Passport** (Snapdragon 801, ARMv7-A). This brings a modern (~2024-era) Chromium engine to a platform that was only ever shipped with an ancient WebKit fork.

## What Works

- **Headless browser** running in single-process mode on the BlackBerry Passport
- **HTML parsing and DOM construction** via Blink
- **V8 JavaScript engine** initialized and running on ARM32 QNX
- **`--dump-dom` output** for `about:blank` and `data:` URLs with clean exit
- **ICU internationalization**, CSS default stylesheets, full DOM tree

```
$ ssh passport "cd /accounts/devuser/berry-deploy && ./content_shell \
    --headless --single-process --no-sandbox --disable-gpu --dump-dom \
    'data:text/html,<h1>Hello from BlackBerry</h1>'" 2>/dev/null
<html><head></head><body><h1>Hello from BlackBerry</h1></body></html>
```

## What Doesn't Work (Yet)

- **HTTP/HTTPS loading** -- Mojo IPC message delivery is unreliable on QNX after the first navigation, blocking the network URL loader pipeline
- **Multi-process mode** -- same Mojo IPC limitation
- **Graphical/windowed mode** -- only headless is implemented
- **Service workers, web workers** -- disabled

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
│   └── qnx-port.patch          # unified diff (324 modified files, ~12k lines)
├── src/                         # 21 new QNX-specific files
│   ├── base/qnx/               # platform abstractions (memory, process, files, threading)
│   ├── build/config/qnx/       # GN platform config (defines, sysroot, flags)
│   ├── build/toolchain/qnx/    # GN toolchain (Clang 17 compile + GCC 9.3 link)
│   ├── content/                 # shell platform delegate, stubs
│   ├── gpu/qnx/                # GPU stubs
│   ├── sandbox/qnx/            # sandbox stubs
│   └── ...
├── build/
│   ├── args.gn                  # GN build arguments
│   └── ld-wrapper.sh            # linker wrapper (LLD + QNX libs)
├── scripts/
│   ├── apply-patches.sh         # apply everything to a Chromium checkout
│   ├── build.sh                 # build content_shell
│   └── run-on-passport.sh       # deploy + run on device
└── docs/
    └── STATUS.md                # detailed status and issue log
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

This applies `patches/qnx-port.patch` and copies the 21 new files into place.

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

### 7. Run

```bash
ssh passport "cd /accounts/devuser/berry-deploy && \
    LD_LIBRARY_PATH=. ./content_shell \
    --headless --single-process --no-sandbox --disable-gpu \
    --disable-features=ServiceWorker --dump-dom \
    'data:text/html,<h1>Hello from BlackBerry</h1>'"
```

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

- **Mojo IPC unreliability**: After the first page load, Mojo messages between browser and renderer components are not delivered reliably. The port bypasses this for `--dump-dom` by dumping the DOM directly from `FrameLoader::FinishedParsing()` during the initial document lifecycle, before any async Mojo communication is needed.

- **FrameLoader uninitialized state**: The initial empty document's `FrameLoader` is in `kUninitialized` state on QNX, preventing the normal load event chain. The port detects this and handles DOM output directly.

- **data: URL handling**: `Shell::LoadURL` converts `data:` URLs to `about:blank` on QNX (since data URL navigation also depends on Mojo). The original data URL content is extracted from the command line and output directly.

- **Single-process only**: Due to QNX process model differences, only `--single-process` mode is supported.

- **Platform stubs**: Missing QNX APIs (sandboxing, GPU, UI platform delegate, etc.) are stubbed out.

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
