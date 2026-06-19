#!/usr/bin/env python3
"""Host side of the RTS/CTS wiring probe (rtschk.exe on the Victor).

Characterises both modem-control directions on the FTDI<->Victor cable so we
know whether RX-side RTS flow control is even physically possible before
building it. See test/rtschk.c for the wire protocol.

Usage: rtschk_host.py [port] [baud]
"""
import sys
import time
import serial

port = sys.argv[1] if len(sys.argv) > 1 else '/dev/ttyUSB0'
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 9600

# Open WITHOUT rtscts so we drive RTS manually for the probe.
s = serial.Serial(port, baud, timeout=2, rtscts=False, dsrdtr=False)
s.dtr = True
s.rts = True
time.sleep(0.2)
s.reset_input_buffer()


def wait_for(expected, timeout=4.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        b = s.read(1)
        if b == expected:
            return True
        if b:                      # a different byte: keep looking briefly
            continue
    return False


# Wait for the Victor's 'R' ready marker.
if not wait_for(b'R', timeout=12.0):
    print("no 'R' ready marker from Victor")
    sys.exit(1)
print("Victor ready.\n")


def set_victor_rts(state):
    """Tell the Victor to (de)assert RTS; wait for its 'A' ack."""
    s.write(b'1' if state else b'0')
    s.flush()
    if not wait_for(b'A'):
        raise RuntimeError("no ack from Victor for RTS set")
    time.sleep(0.05)               # let the line settle


def read_host_cts():
    return bool(s.cts)


def query_victor_cts(host_rts):
    """Set the host's own RTS, ask the Victor what its CTS reads."""
    s.rts = host_rts
    time.sleep(0.05)
    s.write(b'q')
    s.flush()
    b = s.read(1)
    if b not in (b'Y', b'N'):
        raise RuntimeError(f"bad CTS reply from Victor: {b!r}")
    return b == b'Y'


# --- Direction A: Victor RTS -> host CTS (the wire we need) ---------------
print("Direction A  (Victor RTS -> host CTS)  [needed to throttle the PC]")
a_obs = []
for want in (True, False, True, False):
    set_victor_rts(want)
    got = read_host_cts()
    a_obs.append((want, got))
    print(f"  Victor RTS={int(want)}  ->  host CTS={int(got)}")
# Correlation: does host CTS track Victor RTS (either polarity)?
tracks = all(g == w for w, g in a_obs)
inv = all(g != w for w, g in a_obs)
a_wired = tracks or inv
print(f"  => {'WIRED' if a_wired else 'NOT wired'}"
      f"{' (inverted)' if inv else ''}\n")

# --- Direction B: host RTS -> Victor CTS (existing TX-flow path) ----------
print("Direction B  (host RTS -> Victor CTS)  [existing TX-side flow code]")
b_obs = []
for want in (True, False, True, False):
    got = query_victor_cts(want)
    b_obs.append((want, got))
    print(f"  host RTS={int(want)}  ->  Victor CTS={int(got)}")
b_tracks = all(g == w for w, g in b_obs)
b_inv = all(g != w for w, g in b_obs)
b_wired = b_tracks or b_inv
print(f"  => {'WIRED' if b_wired else 'NOT wired'}"
      f"{' (inverted)' if b_inv else ''}\n")

# Restore RTS asserted and tell the Victor to quit.
s.rts = True
s.write(b'Q')
s.flush()
time.sleep(0.2)
s.close()

print("=" * 60)
print(f"Direction A (Victor->host, our throttle wire): "
      f"{'OK' if a_wired else 'MISSING'}")
print(f"Direction B (host->Victor, TX-flow wire):      "
      f"{'OK' if b_wired else 'MISSING'}")
if a_wired:
    print("\nRX-side RTS flow control is VIABLE -> proceed with implementation.")
else:
    print("\nVictor RTS does not reach the host. RX-side hardware flow")
    print("control CANNOT work on this cable; 9600 stays the ceiling")
    print("(unless the cable is rewired or XON/XOFF is used).")
