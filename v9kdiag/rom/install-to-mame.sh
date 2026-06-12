#!/usr/bin/env bash
# Build ram_test.rom and produce a victor9k romset with our image swapped in for
# the FF000 (.8j) BIOS chip, so it can be launched in MAME.
#
# Requires the ORIGINAL victor9k romset (zip). Point at it with VICTOR9K_ZIP, or
# drop victor9k.zip into one of MAME's rompaths. The FE000 (.7j) chip is kept
# as-is; every ".8j" member is replaced with our 4K image.
set -e
cd "$(dirname "$0")"

./build.sh

# MAME keys sets to driver names, so the patched set must still be named
# victor9k.zip; we write it into a dedicated dir that takes rompath precedence.
OUT_DIR="${OUT_DIR:-./mame-roms}"
OUT_ZIP="$OUT_DIR/victor9k.zip"

# locate the original romset
SRC="$VICTOR9K_ZIP"
ORIG_ROMPATH=""
if [ -z "$SRC" ]; then
  for d in "$HOME/mame/roms" /usr/local/share/games/mame/roms /usr/share/games/mame/roms; do
    if [ -f "$d/victor9k.zip" ]; then SRC="$d/victor9k.zip"; ORIG_ROMPATH="$d"; break; fi
  done
fi
if [ -z "$SRC" ] || [ ! -f "$SRC" ]; then
  echo "ERROR: original victor9k romset not found." >&2
  echo "Set VICTOR9K_ZIP=/path/to/victor9k.zip and re-run." >&2
  exit 1
fi

mkdir -p "$OUT_DIR"
python3 - "$SRC" ram_test.rom "$OUT_ZIP" <<'PY'
import sys, zipfile
src, rom, out = sys.argv[1], sys.argv[2], sys.argv[3]
img = open(rom, "rb").read()
assert len(img) == 4096, "ram_test.rom must be 4096 bytes"
zin = zipfile.ZipFile(src)
zout = zipfile.ZipFile(out, "w", zipfile.ZIP_STORED)
swapped = []
for info in zin.infolist():
    data = zin.read(info.filename)
    if info.filename.lower().endswith(".8j"):
        data = img
        swapped.append(info.filename)
    zout.writestr(info.filename, data)
zin.close(); zout.close()
print("Swapped FF000 chip in members:", swapped or "(none found - check romset)")
print("Wrote", out)
PY

echo
echo "Launch with (patched set takes precedence; device roms resolve from the original path):"
echo "  <mame> victor9k -rompath \"$OUT_DIR;${ORIG_ROMPATH:-$HOME/mame/roms}\""
echo
echo "Stock MAME patches two spots in the victor9k ROM after loading; this image is"
echo "laid out so they land in harmless bytes, so stock MAME is fine. The changed"
echo ".8j CRC still triggers a 'ROM is bad' warning screen - dismiss it to boot."
