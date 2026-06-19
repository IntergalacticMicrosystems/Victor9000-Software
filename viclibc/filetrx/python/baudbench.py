#!/usr/bin/env python3
"""
Baud-rate throughput benchmark for filetrx on the real Victor 9000.

Connects at 9600, then for each target baud [9600, 19200, 38400] performs a
live SET_BAUD switch (the Victor acknowledges at the OLD baud, then both sides
flip) and times N PC->Victor transfers of a fixed file. Reports a comparison.

Usage:
    baudbench.py [--port /dev/ttyUSB0] [--file test_xlarge.bin]
                 [--iters 3] [--bauds 9600,19200,38400]
"""
import sys, time, argparse, statistics
sys.path.insert(0, '.')
sys.path.insert(0, '../../serial/python')
from test_suite import TestController, TestConfig, ConnectionType
from v9kfiletrx import TestCmdPacket, COMPRESS_NONE, baud_to_index


def switch_baud(ctrl, target):
    """Live baud switch. Returns True if the link is confirmed at `target`."""
    cur = ctrl.config.baud_rate
    if target == cur:
        return True
    print(f"  switching {cur} -> {target} (SET_BAUD idx {baud_to_index(target)})")
    # Command + response + ACK all complete at the CURRENT baud (Victor
    # reorders: respond first, switch after). send_test_cmd does
    # send_packet then receive_packet (which auto-ACKs and drains TX).
    try:
        resp = ctrl.send_test_cmd(TestCmdPacket.set_baud(baud_to_index(target)))
    except Exception as e:
        print(f"  SET_BAUD command failed: {e}")
        return False
    if not resp.success:
        print(f"  Victor rejected SET_BAUD: {resp.message}")
        return False
    # Host's ACK has drained at the old rate; flip the live port now.
    ser = ctrl.conn._serial
    ser.baudrate = target
    ctrl.conn.baudrate = target
    ctrl.config.baud_rate = target
    ser.reset_input_buffer()
    # Resync sequence bits and confirm the link at the new rate.
    ctrl.pkt.reset()
    for attempt in range(6):
        if ctrl.ping_victor(timeout=3.0):
            print(f"  link confirmed at {target} (ping ok)")
            return True
        ser.reset_input_buffer()
    print(f"  NO PONG at {target} after switch")
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', default='/dev/ttyUSB0')
    ap.add_argument('--file', default='test_xlarge.bin')
    ap.add_argument('--iters', type=int, default=3)
    ap.add_argument('--bauds', default='9600,19200,38400')
    args = ap.parse_args()
    bauds = [int(b) for b in args.bauds.split(',')]

    cfg = TestConfig()
    cfg.connection_type = ConnectionType.REAL_SERIAL
    cfg.serial_port = args.port
    cfg.baud_rate = bauds[0]
    ctrl = TestController(cfg)
    ctrl.generate_test_files()
    if not ctrl.connect():
        print("connect failed"); return 1
    if not ctrl.ping_victor():
        print("no pong from server at startup"); return 1
    f = ctrl.test_dir / args.file
    size = f.stat().st_size
    print(f"\nBenchmark file: {f.name} ({size} bytes), {args.iters} iters/baud, "
          f"compression=NONE\n")

    rows = []
    for baud in bauds:
        if not switch_baud(ctrl, baud):
            rows.append((baud, None, None, None, "switch/link failed"))
            continue
        durs, ok = [], True
        for i in range(args.iters):
            r = ctrl.run_pc_to_victor_test(f, COMPRESS_NONE, i + 1)
            tag = "OK " if r.passed else "FAIL"
            d = r.duration_seconds or 0.0
            bps = (size / d) if (r.passed and d > 0) else 0.0
            if r.passed and d > 0:
                durs.append(d)
            else:
                ok = False
            print(f"  [{baud:5} #{i+1}] {tag} {d:6.2f}s {bps:7.0f} B/s "
                  f"pc_retry={r.pc_retries} vic_retry={r.victor_retries} "
                  f"{r.error_message or ''}")
            # Resync sequence bits between iterations so one bad transfer
            # doesn't desync the rest of the run.
            ctrl.pkt.reset()
        if durs:
            best = min(durs)
            med = statistics.median(durs)
            note = "" if ok else "some failures"
            rows.append((baud, best, med, size / best, note))
        else:
            rows.append((baud, None, None, None, "all transfers failed"))

    print("\n" + "=" * 64)
    print(f"THROUGHPUT COMPARISON  ({f.name}, {size} bytes, comp=NONE)")
    print("=" * 64)
    print(f"{'baud':>7} | {'best s':>7} | {'med s':>7} | {'best B/s':>9} | "
          f"{'% of line':>9} | note")
    print("-" * 64)
    for baud, best, med, bps, note in rows:
        if best is None:
            print(f"{baud:>7} | {'--':>7} | {'--':>7} | {'--':>9} | {'--':>9} | {note}")
            continue
        line_bps = baud / 10.0  # 8N1 = 10 bits/byte
        print(f"{baud:>7} | {best:7.2f} | {med:7.2f} | {bps:9.0f} | "
              f"{100*bps/line_bps:8.1f}% | {note}")
    ctrl.disconnect()
    return 0


if __name__ == '__main__':
    sys.exit(main())
