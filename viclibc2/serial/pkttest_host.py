#!/usr/bin/env python3
"""
Host side of the packet-layer receive test (PKTTEST.EXE on the Victor).

Frames a known incrementing byte pattern into packets that match the viclibc2
packet protocol exactly:

    SYNC(0x7E) | len_lo len_hi | type | payload... | crc_lo crc_hi

where everything after the leading SYNC is byte-stuffed (0x7E/0x7D/0x11/0x13 ->
0x7D, b^0x20) and the CRC is CRC-16/CCITT (poly 0x1021, init 0xFFFF) over
type+payload. The frames are streamed to the Victor over a null-modem link with
a short inter-frame gap: within a frame the bytes go out at line rate (which
stresses the pkt_recv_raw asm hot loop), while the gap lets the Victor finish
its between-frame CRC/copy without overrunning the 3-byte FIFO.

    pkttest_host.py <baudidx> <npackets> <paylen> [port] [gap_ms]

baudidx is a SER_BAUD_* index (6=9600, 7=19200, 8=38400). Start PKTTEST on the
Victor first (it prints 'R' when ready), then run this. Read the guard min/max
and recv/error counts off the Victor console.
"""
import sys
import time
import serial

BAUD_MAP = {0: 110, 1: 300, 2: 600, 3: 1200, 4: 2400,
            5: 4800, 6: 9600, 7: 19200, 8: 38400, 9: 76800}

SYNC = 0x7E
ESC = 0x7D
ESC_XOR = 0x20
XON = 0x11
XOFF = 0x13
TYPE_DATA = 0x01
PKT_TIMEOUT_SHORT = 5000   # guard reset value on the Victor


def crc16_ccitt(data, crc=0xFFFF):
    """CRC-16/CCITT (poly 0x1021, init 0xFFFF, no reflection) - matches the
    table-driven crc16_byte() in packet.c."""
    for b in data:
        crc ^= (b << 8)
        crc &= 0xFFFF
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


def _esc(b, out):
    if b in (SYNC, ESC, XON, XOFF):
        out.append(ESC)
        out.append(b ^ ESC_XOR)
    else:
        out.append(b)


def build_frame(type_, payload):
    crc = crc16_ccitt(bytes([type_]) + payload)
    out = bytearray([SYNC])
    L = len(payload)
    _esc(L & 0xFF, out)
    _esc((L >> 8) & 0xFF, out)
    _esc(type_, out)
    for b in payload:
        _esc(b, out)
    _esc(crc & 0xFF, out)
    _esc((crc >> 8) & 0xFF, out)
    return bytes(out)


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
    baudidx = int(sys.argv[1])
    npackets = int(sys.argv[2])
    paylen = int(sys.argv[3])
    port = sys.argv[4] if len(sys.argv) > 4 else '/dev/ttyUSB0'
    gap = (float(sys.argv[5]) if len(sys.argv) > 5 else 40.0) / 1000.0
    baud = BAUD_MAP[baudidx]

    s = serial.Serial(port, baud, timeout=2, write_timeout=180,
                      rtscts=False, xonxoff=False)
    s.dtr = True
    s.rts = True
    time.sleep(0.2)
    s.reset_input_buffer()
    s.reset_output_buffer()

    print(f"host: port={port} baud={baud} (idx {baudidx}) "
          f"npackets={npackets} paylen={paylen} gap={gap*1000:.0f}ms")

    if not wait_ready(s):
        print("FAIL: no 'R' ready marker from Victor (is PKTTEST running?)")
        s.close()
        sys.exit(1)

    payload = bytes((i & 0xFF) for i in range(paylen))
    f = build_frame(TYPE_DATA, payload)

    # Per-frame handshake: send a frame, then wait for the Victor's one-byte
    # ACK ('A' ok / 'N' crc-or-framing-error) before sending the next. This is
    # a deterministic between-frame gate (no sleep-timing guesswork) so the next
    # frame never streams into the FIFO while the Victor is still processing the
    # previous one. Within a frame the bytes still go out at line rate.
    t0 = time.time()
    wire = 0
    ok = nak = lost = 0
    for i in range(npackets):
        s.write(f)
        s.flush()
        wire += len(f)
        a = s.read(1)
        if a == b'A':
            ok += 1
        elif a == b'N':
            nak += 1
        else:
            lost += 1   # no ACK within the read timeout (Victor desynced/stalled)
    elapsed = time.time() - t0

    rate = (ok * paylen) / elapsed if elapsed else 0
    print(f"  sent {npackets} packets ({paylen} payload each, {wire} wire bytes) "
          f"in {elapsed:.1f}s")
    print(f"  acks: ok={ok} nak={nak} lost={lost}  ({rate:.0f} payload B/s)")

    # After the run the Victor dumps the last packet's full guard array:
    #   'G' | count(2 LE) | count * guard(2 LE)
    # Frame layout of those entries is: [0]=type, [1..len]=payload, [len+1..len+2]=CRC.
    hdr = s.read(1)
    if hdr == b'G':
        cb = s.read(2)
        n = cb[0] | (cb[1] << 8)
        raw = bytearray()
        while len(raw) < n * 2:
            chunk = s.read(n * 2 - len(raw))
            if not chunk:
                break
            raw += chunk
        guards = [raw[2 * i] | (raw[2 * i + 1] << 8) for i in range(len(raw) // 2)]
        slack = [PKT_TIMEOUT_SHORT - g for g in guards]
        print(f"  last-packet guard array: {len(slack)} bytes, "
              f"slack = 5000-guard (idle polls per byte):")
        for off in range(0, len(slack), 20):
            print("   " + " ".join(f"{v:4d}" for v in slack[off:off + 20]))
        nz = [(i, v) for i, v in enumerate(slack) if v != 0]
        print(f"  nonzero slack: {len(nz)} of {len(slack)} positions" +
              ("  -> " + ", ".join(f"[{i}]={v}" for i, v in nz) if nz else " (all zero)"))
    else:
        print(f"  (no guard dump received; got {hdr!r})")
    s.close()


if __name__ == '__main__':
    main()
