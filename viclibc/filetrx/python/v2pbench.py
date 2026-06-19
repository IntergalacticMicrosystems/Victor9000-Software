#!/usr/bin/env python3
"""Victor->PC throughput benchmark across bauds (the asymmetric direction).

PC->Victor is capped at ~9600 by the Victor's RX drain ceiling (the 8088 can
barely service interrupt RX at 9600; 19200 overruns - see baudbench.py and
real-hw-test-result). But Victor->PC has the fast FTDI/Linux host on the
receiving end, so the Victor's TRANSMIT path may sustain 19200/38400. The only
PC->Victor traffic during a Victor->PC transfer is tiny ACK packets, which
already survive at high baud. This measures whether that asymmetry is real.

Live-switches baud (Victor acks at old rate, both flip) and times N
Victor->PC transfers of a resident file.

Usage: v2pbench.py [--file FTXTEST.EXE] [--iters 3] [--bauds 9600,19200,38400]
"""
import sys, time, argparse, statistics
sys.path.insert(0, '.')
sys.path.insert(0, '../../serial/python')
from test_suite import TestController, TestConfig, ConnectionType
from v9kfiletrx import COMPRESS_NONE
from baudbench import switch_baud


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', default='/dev/ttyUSB0')
    ap.add_argument('--file', default='FTXTEST.EXE')
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
    if not ctrl.connect() or not ctrl.ping_victor():
        print("connect/ping failed at startup"); return 1

    print(f"\nVictor->PC source: {args.file}, {args.iters} iters/baud, comp=NONE\n")
    rows = []
    for baud in bauds:
        if not switch_baud(ctrl, baud):
            rows.append((baud, None, None, None, "switch/link failed"))
            continue
        durs, nbytes, ok = [], 0, True
        for i in range(args.iters):
            r = ctrl.run_victor_to_pc_test(args.file, COMPRESS_NONE, i + 1)
            d = r.duration_seconds or 0.0
            n = r.bytes_transferred or 0
            bps = (n / d) if (r.passed and d > 0) else 0.0
            if r.passed and d > 0:
                durs.append(d); nbytes = n
            else:
                ok = False
            print(f"  [{baud:5} #{i+1}] {'OK ' if r.passed else 'FAIL'} "
                  f"{d:6.2f}s {n} bytes {bps:7.0f} B/s "
                  f"pc_retry={r.pc_retries} vic_retry={r.victor_retries} "
                  f"{r.error_message or ''}")
            ctrl.pkt.reset()
        if durs:
            best = min(durs)
            rows.append((baud, best, statistics.median(durs),
                         nbytes / best, "" if ok else "some failures"))
        else:
            rows.append((baud, None, None, None, "all transfers failed"))

    print("\n" + "=" * 64)
    print(f"VICTOR->PC THROUGHPUT  ({args.file}, comp=NONE)")
    print("=" * 64)
    print(f"{'baud':>7} | {'best s':>7} | {'med s':>7} | {'best B/s':>9} | "
          f"{'% of line':>9} | note")
    print("-" * 64)
    for baud, best, med, bps, note in rows:
        if best is None:
            print(f"{baud:>7} | {'--':>7} | {'--':>7} | {'--':>9} | {'--':>9} | {note}")
            continue
        line_bps = baud / 10.0
        print(f"{baud:>7} | {best:7.2f} | {med:7.2f} | {bps:9.0f} | "
              f"{100*bps/line_bps:8.1f}% | {note}")
    # Leave the link back at 9600 for the known-good server state.
    switch_baud(ctrl, 9600)
    ctrl.disconnect()
    return 0


if __name__ == '__main__':
    sys.exit(main())
