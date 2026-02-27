# BAR Deployment for BB10 - Findings & Guide

## What Works

### Shell Deployment (SSH)
- SCP the zip to the device, unzip, `chmod +x`, run
- Binary works from **any directory** on the device
- All flags passed on the command line
- `LD_LIBRARY_PATH=./lib:$LD_LIBRARY_PATH` required to find bundled libs
- stderr shows trace output; use `2>trace.log` to separate from stdout

### BAR Installation
- Installer location: `/accounts/1000/shared/.installer/installer.py`
- Command: `cd /accounts/1000/shared/.installer && python3.2 installer.py <file>.bar`
- Installer extracts BAR, copies to `postBoot` + `current` dirs in `/var/android/`, triggers system install via PPS
- System copies from `current` to `/accounts/1000/appdata/<app-id>/app/native/`
- Navigator launches from the `appdata` path, NOT the `postBoot` path

### BAR File Structure
```
META-INF/
  MANIFEST.MF          # Package metadata + SHA-512 digests
native/
  content_shell         # Main binary (ELF, interpreter: /usr/lib/ldqnx.so.2)
  icudtl.dat           # ICU data
  snapshot_blob.bin    # V8 snapshot
  content_shell.pak    # Content resources
  shell_resources.pak  # Shell resources
  ui_resources_100_percent.pak
  icon.png             # App icon
  bar-descriptor.xml   # App descriptor
  lib/
    libgcc_s.so.1
    libm.so.2
    libstdc++.so.6
    libtest_trace_processor.so
```

### MANIFEST.MF Required Fields
- `Package-Id`: must start with `test` for dev mode (e.g., `testberrybrowsernati667abfda`)
- `Package-Name`: app identifier (e.g., `berrybrowser.native`)
- `Package-Version-Id`: must start with `test` for dev mode
- `Package-Author-Id`: must start with `test` for dev mode
- `Entry-Point`: `LD_LIBRARY_PATH=app/native/lib app/native/content_shell`
- `Entry-Point-Type`: `Qnx/Elf`
- `Archive-Asset-SHA-512-Digest`: SHA-512 digest for each file (base64 encoded)

### Entry-Point Behavior
- Navigator sets CWD to app sandbox root (`/accounts/1000/appdata/<id>/`)
- Binary path is relative: `app/native/content_shell`
- `argv[0]` from navigator has NO path separator (just `content_shell`)
- `LD_LIBRARY_PATH=app/native/lib` is processed by navigator before exec
- No command-line args are passed; must bake defaults into `main()`

## Critical Findings

### DO NOT bundle ldqnx.so.2
- The QNX 8.0 SDK's `ldqnx.so.2` conflicts with BB10's system loader
- Bundling it causes SIGSEGV before `main()` when launched from BAR-installed directories
- Working native BARs (Term49-Enhanced, etc.) do NOT bundle `ldqnx.so.2`
- Use `/usr/lib/ldqnx.so.2` (system default) - this is what the build produces
- `patchelf` is NOT needed since the build already sets the correct interpreter

### DO NOT bundle libsafe_strlen.so
- System provides it; bundling causes conflicts

### BAR Reinstall Gotcha
- Reinstalling over an existing package may silently fail to update `appdata`
- The installer tries `rm -rf` via PPS commands, but if they fail, `shutil.copytree` fails too
- **Fix**: Use a new `Package-Id` for each iteration, OR manually delete old dirs first
- To force a clean install: delete `/var/android/postBoot<pkg-id>` before installing

### chdir Required for Data Files
- When launched from navigator, CWD is the sandbox root, NOT `app/native/`
- Data files (icudtl.dat, etc.) are in `app/native/` relative to sandbox root
- `shell_main.cc` must `chdir("app/native")` when `argv[0]` has no path separator
- Without this, ICU data loading fails and the app crashes

### Logging from BAR
- stderr isn't visible when launched from navigator
- Use `freopen("/accounts/1000/shared/berrybrowser-trace.log", "a", stderr)` for BAR launches
- Only redirect when `argc <= 1` (navigator launch) so SSH runs still show output

## BAR Packaging Script

```python
import hashlib, base64, os

# Generate MANIFEST.MF with SHA-512 digests
elf_files = {'content_shell', 'libgcc_s.so.1', 'libm.so.2', 'libstdc++.so.6', 'libtest_trace_processor.so'}
assets = []
for root, dirs, files in os.walk('native'):
    for f in sorted(files):
        path = os.path.join(root, f)
        with open(path, 'rb') as fh:
            digest = base64.b64encode(hashlib.sha512(fh.read()).digest()).decode()
        assets.append((path, digest, f in elf_files))

# Write manifest, then: zip -r0 app.bar META-INF/ native/
```

## Reference BAR Projects
- `/root/Bar-Context-Manual/` - Various example BAR projects
- `/root/chromium-for-bb10/browsie-browse.bar` - Browsie reference BAR
- Term49-Enhanced - Working native ELF BAR (no Cascades)
