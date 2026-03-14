# Term49 Deployment Guide

Quick deploy of `content_shell` to a BB10 device via Term49 userland.

## Install

```bash
cp /accounts/1000/shared/misc/browser-content-shell-131.zip .
unzip -o browser-content-shell-131.zip
chmod +x bin/content-shell bin/content_shell
```

## Run

```bash
bin/content-shell https://example.com 2>/dev/null
bin/content-shell http://example.com 2>/dev/null
bin/content-shell 'data:text/html,<h1>Hello</h1>' 2>/dev/null
```

## Manual run (without wrapper)

From inside `bin/`:

```bash
cd bin
LD_LIBRARY_PATH=../lib ./content_shell \
  --no-sandbox --disable-gpu --disable-gpu-compositing --no-zygote --single-process \
  --disable-features=ServiceWorker,NetworkServiceDedicatedThread,MojoIpcz,Viz \
  --ozone-platform=headless --headless --dump-dom \
  --ignore-certificate-errors --disable-http2 \
  'https://example.com' 2>/dev/null
```

## Notes

- Resources (`icudtl.dat`, `*.pak`, `snapshot_blob.bin`) must be next to the `content_shell` binary in `bin/`
- `libtest_trace_processor.so` must be in `lib/`
- `--disable-gpu-compositing` and `--disable-features=Viz` are required on Term49 to avoid a compositor crash in `strlen`
- HTTPS requires `--ignore-certificate-errors` (no root CA store) and `--disable-http2` (ALPN issue)
- First run takes ~10-15s for HTTPS, ~5s for HTTP, instant for data: URLs
