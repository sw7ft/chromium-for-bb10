#!/usr/bin/env bash
# Convert a Clang thin archive to a GNU fat archive for QNX ld.bfd.
set -euo pipefail

thin="$(readlink -f "$1")"
out="$(readlink -f "$2")"
cache_root="${PGO_FLAT_ARCHIVE_CACHE:-/tmp/pgi-flat-archives}"

if [[ ! -f "$thin" ]]; then
  echo "qnx-pgi-flatten-archive: missing $thin" >&2
  exit 1
fi

if ! file "$thin" | grep -q 'thin archive'; then
  cp -f "$thin" "$out"
  exit 0
fi

mkdir -p "$cache_root"
mtime="$(stat -c '%Y' "$thin")"
key="$(echo -n "${thin}:${mtime}" | cksum | awk '{print $1}')"
stamp="$cache_root/${key}.stamp"
out_dir="$cache_root/${key}"
out_tmp="${out}.tmp.$$"

if [[ -f "$stamp" ]] && [[ -f "$out" ]]; then
  exit 0
fi

rm -rf "$out_dir"
mkdir -p "$out_dir"

llvm_ar="${LLVM_AR:-llvm-ar-17}"
archive_dir="$(dirname "$thin")"
archive_name="$(basename "$thin")"

members="$(
  cd "$archive_dir"
  "$llvm_ar" t "$archive_name"
)"

while IFS= read -r member; do
  [[ -z "$member" ]] && continue
  safe="${member//\//__}"
  (
    cd "$archive_dir"
    "$llvm_ar" p "$archive_name" "$member"
  ) > "$out_dir/$safe"
done <<< "$members"

rm -f "$out_tmp"
"$llvm_ar" rcs "$out_tmp" "$out_dir"/*.o
mv -f "$out_tmp" "$out"
touch "$stamp"
