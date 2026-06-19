#!/usr/bin/env python3
"""Quick Victor->PC transfer test at a given baud (clean-boot server)."""
import sys, time
sys.path.insert(0, '.'); sys.path.insert(0, '../../serial/python')
from test_suite import TestController, TestConfig, ConnectionType
from v9kfiletrx import COMPRESS_NONE

baud = int(sys.argv[1]) if len(sys.argv) > 1 else 19200
vfile = sys.argv[2] if len(sys.argv) > 2 else "FTX192.EXE"
iters = int(sys.argv[3]) if len(sys.argv) > 3 else 3

cfg = TestConfig()
cfg.connection_type = ConnectionType.REAL_SERIAL
cfg.serial_port = '/dev/ttyUSB0'
cfg.baud_rate = baud
ctrl = TestController(cfg)
ctrl.generate_test_files()
if not ctrl.connect() or not ctrl.ping_victor():
    print("connect/ping failed at", baud); sys.exit(1)
print(f"\nVictor->PC '{vfile}' at {baud} baud, {iters} iters\n")
durs = []
for i in range(iters):
    r = ctrl.run_victor_to_pc_test(vfile, COMPRESS_NONE, i + 1)
    d = r.duration_seconds or 0.0
    n = r.bytes_transferred or 0
    bps = (n / d) if (r.passed and d > 0) else 0.0
    print(f"  [#{i+1}] {'OK ' if r.passed else 'FAIL'} {d:6.2f}s {n} bytes "
          f"{bps:7.0f} B/s pc_retry={r.pc_retries} vic_retry={r.victor_retries} "
          f"{r.error_message or ''}")
    if r.passed and d > 0:
        durs.append((d, n))
    ctrl.pkt.reset()
if durs:
    best_d, n = min(durs, key=lambda x: x[0])
    print(f"\n  BEST {baud}: {n} bytes in {best_d:.2f}s = {n/best_d:.0f} B/s "
          f"({100*(n/best_d)/(baud/10):.1f}% of line rate)")
ctrl.disconnect()
