# igcfs.spec - PyInstaller build recipe for the standalone igcfs executable.
#
# Produces a single self-contained binary (interpreter + pyserial + the vendored
# v9kserial/ and v9kfiletrx/ packages) so end users need no Python install:
#
#     dist/igcfs        (Linux/macOS)
#     dist\igcfs.exe    (Windows)
#
# Build by running, ON THE TARGET OS (PyInstaller cannot cross-compile):
#     pyinstaller igcfs.spec
# or use the build.bat / build.sh wrappers, which set up a venv first.
#
# No hiddenimports are needed: igcfs.py reaches pyserial and every vendored
# submodule through ordinary `import` statements, so PyInstaller's static
# analysis bundles them all. (The lone third-party dep, `serial`, is imported -
# behind a try/except - from v9kserial/serial_port.py.)

a = Analysis(
    ['igcfs.py'],
    pathex=[],
    binaries=[],
    datas=[],
    hiddenimports=[],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    # Trim test-only / GUI weight that the serve path never touches.
    excludes=['tkinter', 'unittest', 'pydoc', 'pytest'],
    noarchive=False,
)

pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='igcfs',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,            # UPX often trips Windows AV false positives; keep raw.
    runtime_tmpdir=None,
    console=True,         # igcfs is a command-line server; keep the console.
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
