#!/usr/bin/env python3
"""
Host side of the pure-polled serial speed test (SPDTEST.EXE on the Victor).

Drives the Victor's Port B / COM2 over a null-modem cable to /dev/ttyUSB0.
The direction argument matches the Victor command exactly:

    spdtest_host.py R <baudidx> <count> [port]   Victor receives, PC sends
    spdtest_host.py T <baudidx> <count> [port]   Victor transmits, PC receives

baudidx is the SER_BAUD_* index (6=9600, 7=19200, 8=38400). Start the matching
SPDTEST run on the Victor first (it prints 'R' when ready), then run this.

Both runs end with the Victor sending a 15-byte report frame:
    'D' recv[4] gaps[4] first_bad[4] rr1[1] 'E'   (u32 little-endian)
recv/gaps/first_bad are the Victor's own accounting; rr1 is the 7201 error
register (bit5 0x20 = receiver overrun). For a TX run the PC additionally
checks the received pattern itself and reports wire throughput.
"""
import sys
import time
import serial

BAUD_MAP = {0: 110, 1: 300, 2: 600, 3: 1200, 4: 2400,
            5: 4800, 6: 9600, 7: 19200, 8: 38400}

FRAME_LEN = 15  # 'D' + 3*u32 + rr1 + 'E'


def read_report(s, timeout=120):
    buf = b''
    t0 = time.time()
    while time.time() - t0 < timeout:
        i = buf.find(b'D')
        if i >= 0 and len(buf) >= i + FRAME_LEN:
            break
        chunk = s.read(64)
        if chunk:
            buf += chunk
    i = buf.find(b'D')
    if i < 0 or len(buf) < i + FRAME_LEN:
        return None
    r = buf[i:i + FRAME_LEN]
    if r[14:15] != b'E':
        return None
    u32 = lambda o: r[o] | (r[o + 1] << 8) | (r[o + 2] << 16) | (r[o + 3] << 24)
    return {'recv': u32(1), 'gaps': u32(5), 'first': u32(9), 'rr1': r[13]}


def wait_ready(s, timeout=15):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if s.read(1) == b'R':
            return True
    return False


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(2)
    direction = sys.argv[1].upper()
    baudidx = int(sys.argv[2])
    count = int(sys.argv[3])
    port = sys.argv[4] if len(sys.argv) > 4 else '/dev/ttyUSB0'
    baud = BAUD_MAP[baudidx]

    s = serial.Serial(port, baud, timeout=2, write_timeout=180, rtscts=False)
    s.dtr = True
    s.rts = True
    time.sleep(0.2)
    s.reset_input_buffer()
    s.reset_output_buffer()

    print(f"host: port={port} baud={baud} (idx {baudidx}) dir={direction} count={count}")

    if not wait_ready(s):
        print("FAIL: no 'R' ready marker from Victor (is SPDTEST running?)")
        s.close()
        sys.exit(1)

    overrun = lambda rr1: bool(rr1 & 0x20)

    if direction == 'R':
        # Victor receives; PC blasts the incrementing pattern at line rate.
        data = bytes((i & 0xFF) for i in range(count))
        t0 = time.time()
        s.write(data)
        s.flush()
        elapsed = time.time() - t0
        rep = read_report(s)
        if not rep:
            print("FAIL: no report frame from Victor")
            s.close()
            sys.exit(1)
        thru = count / elapsed if elapsed else 0
        verdict = "CLEAN" if (rep['recv'] == count and rep['gaps'] == 0
                              and not overrun(rep['rr1'])) else "LOSSY"
        first = 'none' if rep['first'] == 0xFFFFFFFF else rep['first']
        print(f"  sent={count} recv={rep['recv']} gaps={rep['gaps']} "
              f"first_bad={first} rr1=0x{rep['rr1']:02X}"
              f"{' OVERRUN' if overrun(rep['rr1']) else ''}")
        print(f"  wire={elapsed:.2f}s ({thru:.0f} B/s)  -> {verdict}")

    elif direction == 'X':
        # RTS auto-flow probe. The Victor asserts RTS then stops reading so its
        # 3-byte FIFO overflows, never touching RTS in software. We flood it
        # while sampling our CTS (= the Victor's RTS via the crossed null
        # modem). If CTS never drops yet the Victor reports OVERRUN, the 7201
        # did NOT auto-deassert RTS on FIFO-full (RTS is software-only).
        cts_before = s.cts
        cts_low = False
        data = bytes((i & 0xFF) for i in range(count))
        t0 = time.time()
        piece = 8
        for off in range(0, count, piece):
            s.write(data[off:off + piece])
            if not s.cts:
                cts_low = True
        s.flush()
        # Keep sampling CTS across the Victor's stall window.
        while time.time() - t0 < 3.0:
            if not s.cts:
                cts_low = True
        rep = read_report(s)
        drained = rep['recv'] if rep else -1
        ovr = overrun(rep['rr1']) if rep else False
        # The FIFO overflowed iff the Victor drained far fewer bytes than we
        # sent (it was not reading during the burst). That, with CTS staying
        # high, is the robust proof; the RR1 overrun flag is corroboration.
        overflowed = rep is not None and 0 <= drained < count
        print(f"  cts_before={cts_before} cts_dropped_during_burst={cts_low}")
        print(f"  sent={count} victor_drained={drained} (FIFO depth) "
              f"rr1={('0x%02X' % rep['rr1']) if rep else '??'}"
              f"{' OVERRUN' if ovr else ''}")
        if not cts_before:
            print("  -> INCONCLUSIVE: CTS was already low (RTS not asserted / "
                  "line not wired). Check the null-modem RTS/CTS crossover.")
        elif cts_low:
            print("  -> CTS dropped during the burst: something IS deasserting "
                  "RTS (unexpected for a bare 7201).")
        elif overflowed:
            print(f"  -> CONFIRMED: 7201 did NOT auto-deassert RTS on FIFO-full. "
                  f"We flooded {count} bytes into a non-reading receiver; only "
                  f"{drained} survived (the FIFO overflowed, {count - drained} "
                  f"bytes lost) yet our CTS (= Victor RTS) stayed asserted the "
                  f"whole time{' and RR1 shows OVERRUN' if ovr else ''}. "
                  f"RTS is a software-only output on this chip.")
        else:
            print("  -> INCONCLUSIVE: Victor drained ~all bytes; it may have "
                  "been reading. Increase count or stall.")

    elif direction == 'T':
        # Victor transmits; PC says go, then sinks exactly count bytes.
        s.write(b'G')
        s.flush()
        got = bytearray()
        t0 = time.time()
        tlast = t0
        while len(got) < count and time.time() - tlast < 10:
            chunk = s.read(min(4096, count - len(got)))
            if chunk:
                got += chunk
                tlast = time.time()
        elapsed = time.time() - t0
        # Verify the incrementing pattern.
        gaps = 0
        first = None
        for idx, byte in enumerate(got):
            if byte != (idx & 0xFF):
                gaps += 1
                if first is None:
                    first = idx + 1
                break  # one mismatch desyncs the rest; stop counting
        rep = read_report(s)
        thru = len(got) / elapsed if elapsed else 0
        clean = (len(got) == count and gaps == 0)
        verdict = "CLEAN" if clean else "LOSSY"
        print(f"  expected={count} recv={len(got)} "
              f"pattern_gaps={gaps} first_bad={first or 'none'}")
        if rep:
            print(f"  victor_sent={rep['recv']} rr1=0x{rep['rr1']:02X}")
        print(f"  wire={elapsed:.2f}s ({thru:.0f} B/s)  -> {verdict}")
    else:
        print(f"unknown direction {direction!r} (use R or T)")
        s.close()
        sys.exit(2)

    s.close()


if __name__ == '__main__':
    main()
