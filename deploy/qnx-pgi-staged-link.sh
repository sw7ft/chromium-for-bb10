#!/usr/bin/env bash
# Staged content_shell link for QNX PGI (hybrid).
# Partial-link loose .o files only; pass all static archives at the final link.
set -euo pipefail

OUT="${1:-/root/chromium/src/out/qnx-arm-pgi}"
CHUNKS="${PGO_LINK_CHUNKS:-32}"
GXX=/root/qnx800/bin/arm-blackberry-qnx8eabi-g++
PROFILE_RT=/root/llvm-qnx-profile/libclang_rt.profile-arm.a
PART_DIR="$OUT/pgi-link-parts"
RSP="$OUT/content_shell.rsp"
LOOSE_LIST="$PART_DIR/loose.lst"
ARCHIVE_LIST="$PART_DIR/archives.lst"

PARTIAL=(
  -nostdlib
  -r
  -Wl,--allow-multiple-definition
  -B/root/qnx800/bin
  -Wl,--no-gc-sections
)

FINAL=(
  -Wl,--wrap=abort
  -Wl,--allow-multiple-definition
  -Wl,--build-id
  -no-canonical-prefixes
  # Route the final link through the QNX bfd wrapper (flattens Clang thin
  # archives -> fat, then real ld.bfd). Without -fuse-ld=bfd + this -B, g++
  # picks up ld.lld-17, which cannot reconcile the duplicate .L__profd__
  # COMDAT sections produced across the partial-linked chunks and dies with
  # "relocation refers to a symbol in a discarded section".
  -B/root/chromium/src/deploy/qnx-pgi-bin
  -B/root/qnx800/bin
  -Wl,-fuse-ld=bfd
  -Wl,--no-gc-sections
  -L/root/qnx800/arm-blackberry-qnx8eabi/lib
  -L/root/qnx800/arm-blackberry-qnx8eabi/usr/lib
  -fPIC
)

if [[ ! -f "$RSP" ]]; then
  echo "Missing $RSP — run ninja until LINK step generates it." >&2
  exit 1
fi

mkdir -p "$PART_DIR"
cd "$OUT"

tr ' ' '\n' < "$RSP" | awk '/\.a$/{print > a; next} {print > o}' a="$ARCHIVE_LIST" o="$LOOSE_LIST"
loose_count=$(wc -l < "$LOOSE_LIST")
archive_count=$(wc -l < "$ARCHIVE_LIST")
chunk_size=$(( (loose_count + CHUNKS - 1) / CHUNKS ))

echo "Hybrid staged link: $loose_count loose .o + $archive_count archives in $CHUNKS chunks"
rm -f "$PART_DIR"/part_*.o "$PART_DIR"/chunk_*.rsp

parts=()
chunk_idx=0
chunk_rsp="$PART_DIR/chunk_0.rsp"
: > "$chunk_rsp"
count=0

while IFS= read -r obj; do
  echo "$obj" >> "$chunk_rsp"
  count=$((count + 1))
  if [[ $count -ge $chunk_size ]]; then
    part_o="$PART_DIR/part_${chunk_idx}.o"
    echo "[$((chunk_idx + 1))/$CHUNKS] partial link part_${chunk_idx}.o ($count objects)"
    tr '\n' ' ' < "$chunk_rsp" > "${chunk_rsp}.one"
    "$GXX" "${PARTIAL[@]}" -o "$part_o" @"${chunk_rsp}.one"
    parts+=("$part_o")
    chunk_idx=$((chunk_idx + 1))
    count=0
    chunk_rsp="$PART_DIR/chunk_${chunk_idx}.rsp"
    : > "$chunk_rsp"
  fi
done < "$LOOSE_LIST"

if [[ $count -gt 0 ]]; then
  part_o="$PART_DIR/part_${chunk_idx}.o"
  echo "[$((chunk_idx + 1))/$CHUNKS] partial link part_${chunk_idx}.o ($count objects)"
  tr '\n' ' ' < "$chunk_rsp" > "${chunk_rsp}.one"
  "$GXX" "${PARTIAL[@]}" -o "$part_o" @"${chunk_rsp}.one"
  parts+=("$part_o")
fi

final_rsp="$PART_DIR/final.rsp"
{
  for p in "${parts[@]}"; do printf '%s ' "$p"; done
  tr '\n' ' ' < "$ARCHIVE_LIST"
  printf ' %s' "$PROFILE_RT"
} > "$final_rsp"

echo "Final link -> $OUT/exe.unstripped/content_shell"
"$GXX" "${FINAL[@]}" \
  -o "$OUT/exe.unstripped/content_shell" \
  -Wl,--start-group @"$final_rsp" -Wl,--end-group \
  -lm -lpthread -lsocket -lscreen -lbps -lOpenAL -lasound -laudio_manager -lz -lrt

if [[ -x /root/qnx800/bin/arm-blackberry-qnx8eabi-strip ]]; then
  /root/qnx800/bin/arm-blackberry-qnx8eabi-strip \
    -o "$OUT/content_shell" "$OUT/exe.unstripped/content_shell"
fi

ls -lh "$OUT/exe.unstripped/content_shell" "$OUT/content_shell"
