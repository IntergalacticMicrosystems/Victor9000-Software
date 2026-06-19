#!/usr/bin/env python3
"""Host side of the raw RX-integrity probe (rxdiag.exe on the Victor)."""
import sys, time, serial

port = sys.argv[1] if len(sys.argv) > 1 else '/dev/ttyUSB0'
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 19200
N = int(sys.argv[3]) if len(sys.argv) > 3 else 4096
# 4th arg: 'rts'/'1' enables hardware flow control so the host honours the
# Victor's RTS throttle (pace the pattern write to the Victor's drain rate).
rtscts = len(sys.argv) > 4 and sys.argv[4] in ('1', 'rts', 'rtscts', 'flow')

s = serial.Serial(port, baud, timeout=2, write_timeout=90, rtscts=rtscts)
s.dtr = True
if not rtscts:
    s.rts = True
print(f"host rtscts={'ON' if rtscts else 'off'}")
time.sleep(0.2)
s.reset_input_buffer()

# Wait for the Victor's 'R' ready marker.
t0 = time.time()
while time.time() - t0 < 12:
    if s.read(1) == b'R':
        break
else:
    print("no 'R' ready marker from Victor")
    sys.exit(1)

# Send the known pattern (flow control, if on, paces this to the Victor).
data = bytes([i & 0xFF for i in range(N)])
try:
    s.write(data)
    s.flush()
except serial.SerialTimeoutException:
    print("  (write timed out - Victor left RTS deasserted; reading report anyway)")

# Collect the report (framed 'D'...'E').
buf = b''
t0 = time.time()
last = 0
while time.time() - t0 < 90 and b'E' not in buf:
    buf += s.read(64)
    if len(buf) != last:
        print(f"  ...report bytes so far: {len(buf)} @ {time.time()-t0:.0f}s")
        last = len(buf)

i = buf.find(b'D')
if i < 0 or len(buf) < i + 11:
    print(f"no report (got {len(buf)} bytes: {buf!r})")
    sys.exit(1)
r = buf[i:i + 11]
recv = r[1] | (r[2] << 8)
gaps = r[3] | (r[4] << 8)
first = r[5] | (r[6] << 8)
err = r[7]
kicks = r[8] | (r[9] << 8)

errnames = []
for bit, name in ((0x01, 'OVERRUN'), (0x02, 'PARITY'), (0x04, 'FRAMING'),
                  (0x08, 'BREAK'), (0x10, 'BUFOVFL')):
    if err & bit:
        errnames.append(name)

print(f"baud={baud} sent={N} recv={recv} gaps={gaps} "
      f"first_bad={'none' if first == 0xFFFF else first} "
      f"err=0x{err:02X}[{','.join(errnames) or 'none'}] kicks={kicks}")
verdict = "CLEAN" if (recv == N and gaps == 0) else "LOSSY"
print(f"  -> {verdict}: lost/garbled {N - recv} (short) + {gaps} discontinuities; "
      f"{'POLLING (interrupts not firing)' if kicks > N // 4 else 'interrupt-driven'}")
s.close()
