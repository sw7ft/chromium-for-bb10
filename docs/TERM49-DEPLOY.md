# Term49 Deployment Guide

Deploy Chromium 120 `content_shell` to a BB10 device via Term49 userland.

**Build:** Chromium 120.0.6099.234 | V8 12.0.267.17 | ARM32 QNX 8.0

## Quick Install

### Option 1: From PC via SCP

```bash
scp content-shell-term49.zip passport:/accounts/1000/shared/misc/
```

Then in Term49:

```bash
cd /accounts/1000/appdata/com.update.Term49.*/data
cp /accounts/1000/shared/misc/content-shell-term49.zip .
unzip -o content-shell-term49.zip
cd term49-content-shell
chmod +x content_shell run.sh
```

### Option 2: From device file manager

Copy `content-shell-term49.zip` to the device's `misc` shared folder, then in Term49:

```bash
cd /accounts/1000/appdata/com.update.Term49.*/data
cp /accounts/1000/shared/misc/content-shell-term49.zip .
unzip -o content-shell-term49.zip
cd term49-content-shell
chmod +x content_shell run.sh
```

## Usage

```bash
# Hello world test
./run.sh 2>/dev/null

# HTTP
./run.sh http://example.com 2>/dev/null

# HTTPS
./run.sh https://example.com 2>/dev/null

# Inline HTML
./run.sh 'data:text/html,<h1>Hello BB10</h1>' 2>/dev/null

# Save to file
./run.sh http://example.com >page.html 2>/dev/null

# JS-heavy pages (Google, Wikipedia) -- use timeout
./run.sh https://www.google.com --timeout=15000 2>/dev/null

# Faster dump using DOMContentLoaded trigger
./run.sh http://example.com --dom-trigger=domcontentloaded 2>/dev/null

# Debug traces
./run.sh http://example.com --qnx-trace
```

## Manual invocation

If you need full control:

```bash
LD_LIBRARY_PATH=. ./content_shell \
  --no-sandbox --disable-gpu --disable-gpu-compositing \
  --no-zygote --single-process \
  --disable-features=ServiceWorker,NetworkServiceDedicatedThread,MojoIpcz,Viz \
  --ozone-platform=headless --headless --dump-dom \
  --ignore-certificate-errors --disable-http2 \
  --timeout=15000 \
  'https://example.com' 2>/dev/null
```

## Flags

| Flag | Purpose |
|------|---------|
| `--timeout=<ms>` | Force DOM dump after N ms (prevents hangs on JS-heavy pages) |
| `--dom-trigger=domcontentloaded` | Dump on DOMContentLoaded instead of window.onload |
| `--qnx-trace` | Verbose QNX debug output on stderr |
| `--ignore-certificate-errors` | Skip TLS certificate verification |
| `--disable-http2` | Force HTTP/1.1 (avoids ALPN 400 errors from some CDNs) |

## What works

| URL type | Example | Status |
|----------|---------|--------|
| data: URLs | `data:text/html,<h1>Hi</h1>` | Working |
| HTTP | `http://example.com` | Working |
| HTTPS | `https://example.com` | Working |
| JS-heavy (w/ timeout) | `https://www.google.com --timeout=15000` | Working |

## Performance

- data: URLs: ~2 seconds
- HTTP: ~5-10 seconds
- HTTPS: ~10-15 seconds
- `--dom-trigger=domcontentloaded` saves 2-5 seconds on complex pages
- `--timeout=15000` recommended for any JS-heavy site

## Notes

- All files must be in the same directory (flat layout)
- `--disable-gpu-compositing` and `--disable-features=Viz` are required on Term49 to avoid a viz.mojom.Gpu crash
- First HTTPS request is slower due to TLS handshake
- If `cacert.pem` is present, `run.sh` uses it for proper cert verification; otherwise falls back to `--ignore-certificate-errors`

## Troubleshooting

| Problem | Fix |
|---------|-----|
| `ldd:FATAL: Could not load library` | Ensure LD_LIBRARY_PATH=. or use run.sh |
| `No such file or directory` on exec | Run `chmod +x content_shell run.sh` |
| HTTPS hangs forever | Add `--timeout=15000` |
| `Memory fault (core dumped)` on HTTPS | Ensure `--disable-gpu-compositing --disable-features=Viz` flags present |
| ICU data not found | Ensure `icudtl.dat` is in the same directory as the binary |
