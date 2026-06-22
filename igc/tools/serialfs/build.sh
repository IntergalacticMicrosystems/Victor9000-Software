#!/usr/bin/env bash
# Build the standalone igcfs binary on Linux or macOS.
#
# Produces dist/igcfs, a single file with no Python dependency:
#     ./dist/igcfs --root /srv/victor --dev /dev/ttyUSB0 --baud 38400 -v
#
# PyInstaller cannot cross-compile: run this on the OS you want a binary for.
# (For a Windows .exe, run build.bat on Windows instead.)
set -euo pipefail
cd "$(dirname "$0")"

if [ ! -d .build-venv ]; then
    echo "Creating build venv..."
    python3 -m venv .build-venv
fi

# shellcheck disable=SC1091
source .build-venv/bin/activate
python -m pip install --quiet --upgrade pip
python -m pip install --quiet pyserial pyinstaller

echo "Building igcfs..."
pyinstaller --clean --noconfirm igcfs.spec

echo
echo "Done. Executable: dist/igcfs"
