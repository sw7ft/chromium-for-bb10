#!/usr/bin/env python3
# Generates *_shim_script.h from *.js with node --check validation.
import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> int:
  if len(sys.argv) not in (3, 7):
    print(
        'usage: gen_shim_header.py INPUT.js OUTPUT.h '
        '[PLACEHOLDER DUMMY CONST_NAME DELIM]',
        file=sys.stderr,
    )
    return 1

  src = Path(sys.argv[1])
  out = Path(sys.argv[2])
  js = src.read_text(encoding='utf-8')

  if len(sys.argv) == 7:
    placeholder = sys.argv[3]
    dummy = sys.argv[4]
    const_name = sys.argv[5]
    delim = sys.argv[6]
  else:
    placeholder = '__BERRY_VIDEO_ID__'
    dummy = 'jNQXAC9IVRw'
    const_name = 'kBerryWatchShimScript'
    delim = 'BERRY_WATCH_SHIM'

  if placeholder not in js:
    print(f'error: {src} missing {placeholder}', file=sys.stderr)
    return 1

  check_js = js.replace(placeholder, dummy)
  with tempfile.NamedTemporaryFile('w', suffix='.js', delete=False) as tmp:
    tmp.write(check_js)
    tmp_path = tmp.name
  try:
    proc = subprocess.run(['node', '--check', tmp_path], capture_output=True)
  finally:
    Path(tmp_path).unlink(missing_ok=True)
  if proc.returncode != 0:
    sys.stderr.write(proc.stderr.decode('utf-8', errors='replace'))
    print(f'error: node --check failed for {src}', file=sys.stderr)
    return proc.returncode

  if f'){delim}"' in js:
    print(f'error: JS contains raw-string delimiter ){delim}"', file=sys.stderr)
    return 1

  guard = f'SERVICES_NETWORK_QNX_{delim}_SCRIPT_H_'
  header = f'''// Copyright 2026 BerryBrowserV3 contributors
// Generated from {src.name} — DO NOT EDIT.

#ifndef {guard}
#define {guard}

namespace network {{

inline constexpr char {const_name}[] = R"{delim}(
{js}){delim}";

}}  // namespace network

#endif  // {guard}
'''
  out.parent.mkdir(parents=True, exist_ok=True)
  out.write_text(header, encoding='utf-8')
  print(f'gen_shim_header: OK {src} -> {out} ({len(js)} bytes JS)')
  return 0


if __name__ == '__main__':
  sys.exit(main())
