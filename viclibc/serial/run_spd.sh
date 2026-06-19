#!/bin/bash
# Orchestrate one polled-serial speed test run.
#   run_spd.sh <R|T> <baudidx> <count> [i]
# Starts the PC host harness on ttyUSB0 (listening for the Victor's 'R'),
# then types the SPDTEST command on the Victor via the remote control.
set -u
DIR="$1"; BAUD="$2"; COUNT="$3"; FLAG="${4:-}"
RC=/root/sync/Victor9000-Development-Private/victor9k_remote_control
SER=/root/sync/Victor9000-Development-Private/viclibc/serial
PY=$RC/host/.venv/bin/python   # has pyserial

OUT=$(mktemp)
# Host harness listens first (waits up to 15s for 'R').
$PY "$SER/spdtest_host.py" "$DIR" "$BAUD" "$COUNT" >"$OUT" 2>&1 &
HPID=$!
# Let it open the port before the Victor emits 'R'.
$PY -c "import time;time.sleep(1.0)"
# Build the DOS command line (letters/digits/space only - emulator-safe).
# Append the two-char escape \n (remotectl interprets it on-device as Enter);
# a literal newline byte is NOT treated as Enter.
CMD="SPDTEST $DIR $BAUD $COUNT"
[ -n "$FLAG" ] && CMD="$CMD $FLAG"
timeout 30 $PY "$RC/host/remotectl.py" type "$CMD\n" >/dev/null 2>&1
wait $HPID
echo "----- HOST ($DIR baud=$BAUD count=$COUNT flag=${FLAG:-none}) -----"
cat "$OUT"
rm -f "$OUT"
