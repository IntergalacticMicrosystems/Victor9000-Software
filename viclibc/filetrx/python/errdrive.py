#!/usr/bin/env python3
"""Focused error-recovery driver: one PC->Victor transfer per case, timed."""
import sys, time
sys.path.insert(0, '.')
sys.path.insert(0, '../../serial/python')
from test_suite import TestController, TestConfig, ConnectionType
from v9kfiletrx import ErrorPresets, COMPRESS_NONE

cfg = TestConfig()
cfg.connection_type = ConnectionType.REAL_SERIAL
cfg.serial_port = '/dev/ttyUSB0'
cfg.baud_rate = 9600
ctrl = TestController(cfg)
if not ctrl.connect():
    print("connect failed"); sys.exit(1)
if not ctrl.ping_victor():
    print("no pong"); sys.exit(1)
print("server ready\n")

f = ctrl.test_dir / (sys.argv[1] if len(sys.argv) > 1 else 'test_small.bin')
cases = [
    ('baseline',        None),
    ('single_drop',     ErrorPresets.single_drop()),
    ('single_corrupt',  ErrorPresets.single_corrupt()),
    ('multiple_drops',  ErrorPresets.multiple_drops()),
]
for label, preset in cases:
    t0 = time.time()
    res = ctrl.run_pc_to_victor_test(f, COMPRESS_NONE, 1, error_config=preset)
    print(f"[{label:15}] passed={res.passed} dur={time.time()-t0:5.1f}s "
          f"pc_retries={res.pc_retries} victor_retries={res.victor_retries} "
          f"msg={res.error_message}")
ctrl.disconnect()
