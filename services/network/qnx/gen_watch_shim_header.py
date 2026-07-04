#!/usr/bin/env python3
# Generates watch_shim_script.h from watch_shim.js with node --check validation.
import subprocess
import sys
from pathlib import Path

PLACEHOLDER = '__BERRY_VIDEO_ID__'
DUMMY_ID = 'jNQXAC9IVRw'
DELIM = 'BERRY_WATCH_SHIM'


def main() -> int:
  if len(sys.argv) != 3:
    print('usage: gen_watch_shim_header.py INPUT.js OUTPUT.h', file=sys.stderr)
    return 1

  src = Path(sys.argv[1])
  out = Path(sys.argv[2])
  js = src.read_text(encoding='utf-8')

  if PLACEHOLDER not in js:
    print(f'error: {src} missing {PLACEHOLDER}', file=sys.stderr)
    return 1

  check_js = js.replace(PLACEHOLDER, DUMMY_ID)
  import tempfile
  with tempfile.NamedTemporaryFile('w', suffix='.js', delete=False) as tmp:
    tmp.write(check_js)
    tmp_path = tmp.name
  try:
    proc = subprocess.run(
        ['node', '--check', tmp_path],
        capture_output=True,
    )
  finally:
    Path(tmp_path).unlink(missing_ok=True)
  if proc.returncode != 0:
    sys.stderr.write(proc.stderr.decode('utf-8', errors='replace'))
    print(f'error: node --check failed for {src}', file=sys.stderr)
    return proc.returncode

  if f'){DELIM}"' in js:
    print(f'error: JS contains raw-string delimiter ){DELIM}"', file=sys.stderr)
    return 1

  header = f'''// Copyright 2026 BerryBrowserV3 contributors
// Generated from {src.name} — DO NOT EDIT.

#ifndef SERVICES_NETWORK_QNX_WATCH_SHIM_SCRIPT_H_
#define SERVICES_NETWORK_QNX_WATCH_SHIM_SCRIPT_H_

namespace network {{

inline constexpr char kBerryWatchShimScript[] = R"{DELIM}(
{js}){DELIM}";

}}  // namespace network

#endif  // SERVICES_NETWORK_QNX_WATCH_SHIM_SCRIPT_H_
'''
  out.parent.mkdir(parents=True, exist_ok=True)
  out.write_text(header, encoding='utf-8')
  print(f'gen_watch_shim_header: OK {src} -> {out} ({len(js)} bytes JS)')
  return 0


if __name__ == '__main__':
  sys.exit(main())
