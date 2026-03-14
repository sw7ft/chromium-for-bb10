# Chromium content_shell for BlackBerry 10

A working build of Chromium's `content_shell` for QNX 8.0.0 / ARMv7-A,
tested on the BlackBerry Passport.

Runs in **headless mode** and dumps the rendered DOM to stdout.

## What's Included

| File | Description |
|------|-------------|
| `content_shell` | Main binary (ELF 32-bit ARM, ~91 MB) |
| `content_shell.pak` | Chromium content resources |
| `shell_resources.pak` | Shell UI resources |
| `ui_resources_100_percent.pak` | UI bitmap resources |
| `icudtl.dat` | ICU Unicode data |
| `snapshot_blob.bin` | V8 JavaScript engine snapshot |
| `libgcc_s.so.1` | GCC runtime (QNX 8.0.0 toolchain) |
| `libstdc++.so.6` | C++ standard library (QNX 8.0.0 toolchain) |
| `libm.so.2` | Math library (QNX 8.0.0 toolchain) |
| `run.sh` | Convenience launcher script |
| `www/index.html` | Sample local test page |

## Quick Start

### 1. Copy to the device

```bash
scp -r bb10-content-shell/ user@device:/accounts/devuser/bb10-content-shell/
```

Or if you downloaded the tarball:

```bash
# Extract on your host machine
tar xzf bb10-content-shell.tar.gz

# Copy to device
scp -r bb10-content-shell/ user@device:/accounts/devuser/bb10-content-shell/
```

### 2. SSH into the device

```bash
ssh user@device
cd /accounts/devuser/bb10-content-shell
```

### 3. Run it

**Inline HTML (data: URL):**

```bash
./run.sh
# Output: <html><head></head><body><h1>Hello from BB10</h1></body></html>
```

**Custom data: URL:**

```bash
./run.sh 'data:text/html,<h1>Custom</h1><p>Content here</p>'
```

**External website:**

```bash
./run.sh http://example.com
# Output: full DOM of example.com
```

**Local HTTP server:**

```bash
# Terminal 1: start a web server
cd www
python3.2 -m http.server 8001 &

# Terminal 2: fetch the page
./run.sh http://127.0.0.1:8001/index.html
```

**Suppress debug traces (clean output):**

```bash
./run.sh http://example.com 2>/dev/null
```

**Save output to a file:**

```bash
./run.sh http://example.com >output.html 2>/dev/null
```

## Manual Invocation

If you need finer control, run `content_shell` directly:

```bash
export LD_LIBRARY_PATH=.:$LD_LIBRARY_PATH

./content_shell \
  --no-sandbox \
  --disable-gpu \
  --no-zygote \
  --single-process \
  --disable-features=ServiceWorker,NetworkServiceDedicatedThread,MojoIpcz \
  --ozone-platform=headless \
  --headless \
  --dump-dom \
  'http://example.com'
```

### Required flags

| Flag | Why |
|------|-----|
| `--no-sandbox` | QNX doesn't support Linux namespaces |
| `--disable-gpu` | No GPU compositing in headless |
| `--no-zygote` | QNX doesn't support fork-based zygote |
| `--single-process` | Renderer runs in the browser process |
| `--ozone-platform=headless` | Prevents QNX Screen event loop from spinning |
| `--headless --dump-dom` | Headless mode, prints DOM to stdout |
| `--disable-features=...` | Disables subsystems with QNX incompatibilities |

## Verified Working

| URL Type | Example | Status |
|----------|---------|--------|
| `data:` URLs | `data:text/html,<h1>Hi</h1>` | Working |
| Local HTTP | `http://127.0.0.1:8001/` | Working |
| External HTTP | `http://example.com` | Working |

## Known Limitations

- **Headless only**: No graphical window (QNX Screen platform exists but is experimental)
- **Single-process mode**: Browser and renderer share one process
- **No HTTPS**: TLS/SSL has not been tested yet
- **No JavaScript-heavy sites**: Complex JS may hit memory limits on the Passport's 3 GB RAM
- **ARM32 stdlib bugs**: Several `std::string` methods are broken on QNX ARM and have been patched with manual workarounds in this build

## System Requirements

- BlackBerry 10 device (tested on Passport with OS 10.3.x / QNX 8.0.0)
- SSH access to the device
- ~120 MB free storage
