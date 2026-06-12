#!/usr/bin/env bash
# Build the Victor 9000 RAM test ROM (4K, FF000 socket).
set -e
cd "$(dirname "$0")"
NASM=${NASM:-nasm}

"$NASM" -f bin -O0 ram_test.asm -o ram_test.rom -l ram_test.lst

SIZE=$(stat -c%s ram_test.rom)
if [ "$SIZE" -ne 4096 ]; then
  echo "ERROR: ram_test.rom is $SIZE bytes, expected exactly 4096" >&2
  exit 1
fi

echo "Built ram_test.rom (4096 bytes)."
