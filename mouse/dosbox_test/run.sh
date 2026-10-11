#!/bin/sh
# DOSBox smoke test for MOUSE.COM's install path (DOSBox is a PC: the Victor I/O at E000h is plain
# memory here, so this tests the installer logic, not the 7201 driver). Needs nasm and dosbox.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${TMPDIR:-/tmp}/mouse_com_dosbox_test
rm -rf "$WORK"; mkdir -p "$WORK"
nasm -f bin "$HERE/harness.asm" -o "$WORK/TEST.COM"
cp "$HERE/../MOUSE.COM" "$WORK/MOUSE.COM"
cat > "$WORK/test.conf" <<CONF
[sdl]
output=surface
[dosbox]
memsize=1
[autoexec]
mount c $WORK
c:
test
exit
CONF
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 90 dosbox -conf "$WORK/test.conf" -noconsole >/dev/null 2>&1 || true
OUT=$(tr -d '\r' < "$WORK/OUT.TXT")
echo "$OUT"
echo "--- checks"
fail=0
chk() { if echo "$OUT" | grep -q -- "$1"; then echo "ok   $2"; else echo "FAIL $2"; fail=1; fi; }
chk "Usage:  MOUSE A"                          "usage screen when no letter is given"
chk "No mouse detected on serial port A"        "no-mouse message on the empty port"
chk "MOUSE is not installed."                   "no-mouse run says it did not install"
chk "child rc=0002"                             "no-mouse run returns errorlevel 2"
chk "another INT 33h mouse handler"            "foreign handler notice (DOSBox's own INT 33h) on the real install"
chk "installed for serial port A."              "sign-on names port A"
chk "MOUSE is already installed."               "second load refused"
[ "$(echo "$OUT" | grep -c 'child rc=0001')" = 2 ] && echo "ok   usage and refused load return errorlevel 1" || { echo "FAIL rc 0001 count"; fail=1; }
[ "$(echo "$OUT" | grep -c 'child rc=0300')" = 1 ] && echo "ok   exactly one TSR install (rc 0300)" || { echo "FAIL TSR install count"; fail=1; }
[ "$(echo "$OUT" | grep -c ':[0-9A-F]\{4\} MOUSE ')" = 2 ] && echo "ok   INT 33h carries the MOUSE signature only after the real install" || { echo "FAIL signature count"; fail=1; }
# INT 41h must be identical before run 2 and after it (the no-mouse path restores it)
V41=$(echo "$OUT" | sed -n 's/.*INT41=\([0-9A-F:]*\).*/\1/p')
[ "$(echo "$V41" | sed -n 2p)" = "$(echo "$V41" | sed -n 3p)" ] && echo "ok   INT 41h restored after the no-mouse run" || { echo "FAIL INT 41h not restored"; fail=1; }
[ "$(echo "$V41" | sed -n 3p)" != "$(echo "$V41" | sed -n 4p)" ] && echo "ok   INT 41h hooked by the real install" || { echo "FAIL INT 41h not hooked"; fail=1; }
exit $fail
