#!/usr/bin/env python3
"""
Victor 9000 Serial Library Automated Test Suite (host side)

Drives the test agent (test/sertest.c -> sertest.exe) running on the
Victor 9000 and asserts on the results. Together the two sides exercise
every public function in serial.h and packet.h, including protocol
misbehavior (corrupt CRC, duplicate sequence, withheld ACK, oversize
packets) and ISR-level behaviors (buffer overflow, XON/XOFF suspension).

Usage:
    1. Start MAME with PTY enabled:
       ./mame victor9k -rs232a pty

    2. On the Victor, run the agent:
       sertest            (or: sertest 9600)

    3. Run this suite (auto-detects MAME's PTY):
       python3 victor9k_serial_test.py

Other modes:
    victor9k_serial_test.py --port /dev/pts/5     # explicit port
    victor9k_serial_test.py --selftest            # validate the harness
                                                  # against a local fake
                                                  # agent (no MAME needed)
    victor9k_serial_test.py --list                # list tests + coverage
    victor9k_serial_test.py --only crc16,ping     # run a subset
    victor9k_serial_test.py --quit                # shut the agent down
                                                  # at the end (covers
                                                  # ser_shutdown)

The command opcodes and payload layouts below MUST be kept in sync with
test/sertest.c.
"""

import argparse
import os
import struct
import sys
import threading
import time
import traceback

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                'python'))

from v9kserial import (
    Connection, SerialConnection, MamePtyConnection,
    PacketProtocol, PacketError,
)
from v9kserial.constants import (
    PKT_SYNC, PKT_ESC, PKT_ESC_XOR,
    PKT_TYPE_DATA, PKT_TYPE_ACK, PKT_TYPE_NAK,
    PKT_SEQ_BIT, PKT_TYPE_MASK, PKT_MAX_PAYLOAD,
    BAUD_RATES, XON, XOFF,
)

# ---------------------------------------------------------------------------
# Agent command opcodes (keep in sync with test/sertest.c)
# ---------------------------------------------------------------------------

CMD_PING = 0x01
CMD_ECHO = 0x02
CMD_CRC = 0x03
CMD_STATUS = 0x04
CMD_RAW_ECHO = 0x05
CMD_WRITE_STR = 0x06
CMD_WRITE_BLOCK = 0x07
CMD_READ_BLOCK = 0x08
CMD_RX_FLUSH = 0x09
CMD_OVERFLOW = 0x0A
CMD_FLUSH_TX = 0x0B
CMD_FLOW_XONOFF = 0x0C
CMD_FLOW_SMOKE = 0x0D
CMD_BAUD_BAD = 0x0E
CMD_BAUD_CYCLE = 0x0F
CMD_FORMAT_BAD = 0x10
CMD_FORMAT_CYCLE = 0x11
CMD_INIT_PORT = 0x12
CMD_LINES = 0x13
CMD_RX_READY = 0x14
CMD_PKT_PARAMS = 0x15
CMD_STATS = 0x16
CMD_HWSTATE = 0x17
CMD_QUIT = 0x7F

AGENT_VER_MAJOR = 2

SER_BUF_SIZE = 2048
SER_ERR_BUFOVFL = 0x10

# Baud index constants (mirror SER_BAUD_*)
BAUD_IDX = {110: 0, 300: 1, 600: 2, 1200: 3,
            2400: 4, 4800: 5, 9600: 6, 19200: 7}


def pat_bytes(seed: int, n: int) -> bytes:
    """Deterministic pattern shared with the agent: printable ASCII
    0x20..0x7C (never SYNC/ESC/XON/XOFF)."""
    return bytes(0x20 + ((seed + i) % 0x5D) for i in range(n))


def esc_byte(b: int) -> bytes:
    if b in (PKT_SYNC, PKT_ESC, XON, XOFF):
        return bytes([PKT_ESC, b ^ PKT_ESC_XOR])
    return bytes([b])


def build_raw_packet(type_byte: int, payload: bytes = b'',
                     corrupt_crc: bool = False,
                     length_override: int = None,
                     truncate_after_length: bool = False) -> bytes:
    """Hand-build a wire packet, optionally malformed, for the
    protocol-misbehavior tests."""
    crc = PacketProtocol.crc16(bytes([type_byte]) + payload)
    if corrupt_crc:
        crc ^= 0xFFFF

    length = len(payload) if length_override is None else length_override

    out = bytearray([PKT_SYNC])
    out += esc_byte(length & 0xFF)
    out += esc_byte((length >> 8) & 0xFF)
    if truncate_after_length:
        return bytes(out)
    out += esc_byte(type_byte)
    for b in payload:
        out += esc_byte(b)
    out += esc_byte(crc & 0xFF)
    out += esc_byte((crc >> 8) & 0xFF)
    return bytes(out)


# ---------------------------------------------------------------------------
# Test plumbing
# ---------------------------------------------------------------------------

class TestFailure(Exception):
    pass


class TestSkip(Exception):
    pass


def check(cond, msg):
    if not cond:
        raise TestFailure(msg)


class AgentClient:
    """Command/response client for the Victor-side test agent."""

    def __init__(self, conn: Connection, scale: float = 1.0):
        self.conn = conn
        self.proto = PacketProtocol(conn, timeout=3.0)
        self.scale = scale

    def t(self, seconds: float) -> float:
        """Scale a wait that tracks the agent's nominal clock."""
        return max(0.05, seconds * self.scale)

    # --- normal command flow ---

    def send_cmd(self, op: int, args: bytes = b''):
        self.proto.send_packet(bytes([op]) + args, timeout=self.t(4.0))

    def recv_rsp(self, op: int, timeout: float) -> tuple:
        """Receive one agent response; returns (status, data)."""
        resp = self.proto.receive_packet(timeout=timeout)
        check(len(resp) >= 2, f"short response: {resp.hex()}")
        check(resp[0] == op,
              f"response opcode 0x{resp[0]:02X}, expected 0x{op:02X}")
        return resp[1], resp[2:]

    def command(self, op: int, args: bytes = b'',
                timeout: float = None, expect_status: int = 0) -> bytes:
        if timeout is None:
            timeout = self.t(8.0)
        self.send_cmd(op, args)
        status, data = self.recv_rsp(op, timeout)
        if expect_status is not None:
            check(status == expect_status,
                  f"cmd 0x{op:02X}: status {status}, "
                  f"expected {expect_status}")
        return data

    def stats(self) -> dict:
        d = self.command(CMD_STATS)
        check(len(d) >= 7, "short STATS response")
        cmds, pings, rc_u16 = struct.unpack('<HHH', d[:6])
        rc = rc_u16 - 0x10000 if rc_u16 >= 0x8000 else rc_u16
        kicks = struct.unpack('<H', d[7:9])[0] if len(d) >= 9 else 0
        return {'cmds': cmds, 'pings': pings,
                'last_rc': rc, 'pkt_err': d[6], 'kicks': kicks}

    # --- raw-phase helpers ---

    def read_exact(self, n: int, timeout: float) -> bytes:
        data = self.conn.receive(n, timeout=timeout)
        check(len(data) == n,
              f"raw read: got {len(data)}/{n} bytes")
        return data

    def read_until_sync(self, timeout: float) -> bytes:
        """Read raw bytes up to (excluding) the next SYNC; the SYNC is
        pushed back for the packet layer."""
        data = self.conn.receive_until(bytes([PKT_SYNC]), timeout=timeout)
        check(data.endswith(bytes([PKT_SYNC])),
              f"no SYNC within timeout (got {len(data)} bytes)")
        self.conn.unread(bytes([PKT_SYNC]))
        return data[:-1]

    def drain(self, settle: float = 0.05) -> bytes:
        return self.conn.receive_available(timeout=settle)

    def resync(self) -> bool:
        """Try to recover protocol sequence state after a failed test."""
        self.drain(0.3)
        for _ in range(4):
            try:
                self.command(CMD_PING, timeout=2.5)
                return True
            except (TestFailure, PacketError):
                # Try the other sequence-bit combination
                self.proto._seq_tx ^= 1
                self.drain(0.3)
        return False


# ---------------------------------------------------------------------------
# Test registry
# ---------------------------------------------------------------------------

SUITE = []

# Canonical public API, used for the coverage report
SERIAL_H_FUNCS = [
    'ser_init', 'ser_init_port', 'ser_shutdown',
    'ser_set_baud', 'ser_set_format', 'ser_set_flow',
    'ser_rx_ready', 'ser_tx_ready',
    'ser_int_enable', 'ser_int_disable',
    'ser_int_read', 'ser_int_write',
    'ser_int_rx_available', 'ser_int_tx_free',
    'ser_int_flush_rx', 'ser_int_flush_tx', 'ser_int_drain_tx',
    'ser_int_read_block', 'ser_int_write_block', 'ser_write_str',
    'ser_get_error', 'ser_check_cts', 'ser_check_dcd',
    'ser_set_rts', 'ser_set_dtr', 'ser_get_baud',
]
PACKET_H_FUNCS = [
    'pkt_crc16', 'pkt_crc16_byte', 'pkt_init',
    'pkt_set_timeout', 'pkt_set_retries',
    'pkt_send_raw', 'pkt_recv_raw', 'pkt_send', 'pkt_receive',
    'pkt_send_ack', 'pkt_send_nak',
    'pkt_get_error', 'pkt_clear_error', 'pkt_flush', 'pkt_wait_sync',
]


def test(name, covers=(), needs_quit=False):
    def deco(fn):
        SUITE.append({'name': name, 'fn': fn, 'covers': list(covers),
                      'needs_quit': needs_quit, 'doc': fn.__doc__ or ''})
        return fn
    return deco


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

@test('ping',
      covers=['ser_init', 'pkt_init', 'pkt_send', 'pkt_receive',
              'pkt_send_raw', 'pkt_recv_raw', 'pkt_send_ack',
              'pkt_wait_sync'])
def t_ping(c: AgentClient):
    """Agent liveness and protocol round trip."""
    d = c.command(CMD_PING)
    check(len(d) >= 6, "short PING response")
    check(d[0] == AGENT_VER_MAJOR,
          f"agent major version {d[0]}, expected {AGENT_VER_MAJOR} "
          "(rebuild sertest.exe?)")
    return f"agent v{d[0]}.{d[1]}"


@test('int_health')
def t_int_health(c: AgentClient):
    """Interrupt-chain diagnostic (informational; never asserts).
    Parks a byte in the SIO FIFO while the agent is not polling, then
    reads back CPU IF, PIC IMR/IRR/ISR and SIO status in one snapshot."""
    c.send_cmd(CMD_HWSTATE)
    time.sleep(0.05)
    c.conn.send(b'I')   # the probe byte that should raise the interrupt
    status, d = c.recv_rsp(CMD_HWSTATE, c.t(20.0))
    check(status == 0, f"status {status}")
    if_bit, imr, irr, isr, rr0a, rr0b = d[:6]
    rxavail, kicks = struct.unpack('<HH', d[6:10])

    raw = (f"IF={if_bit} IMR={imr:02X} IRR={irr:02X} ISR={isr:02X} "
           f"RR0A={rr0a:02X} RR0B={rr0b:02X} rxbuf={rxavail} "
           f"kicks={kicks}")

    pending = rr0a & 0x01           # probe byte still in SIO FIFO
    if rxavail > 0 and not pending:
        verdict = "interrupts ALIVE (ISR consumed the probe byte)"
    elif if_bit == 0:
        verdict = "CPU IF=0: interrupts globally disabled!"
    elif imr & 0x02:
        verdict = "PIC has the SIO line MASKED (IMR bit 1 set)"
    elif pending and not (irr & 0x02) and not (isr & 0x02):
        verdict = ("SIO has data + int pending but PIC IRR clear: "
                   "INT line not reaching the PIC "
                   "(SIO int enables / wiring / polarity)")
    elif pending and (isr & 0x02):
        verdict = ("PIC stuck IN-SERVICE on the SIO line: an earlier "
                   "interrupt was never EOI'd")
    elif pending and (irr & 0x02):
        verdict = ("PIC sees the request but the CPU never takes it: "
                   "vector 0x41 hook or PIC base mismatch")
    else:
        verdict = "inconclusive (probe byte may have been missed)"
    return f"{verdict} [{raw}]"


@test('echo_ascii')
def t_echo_ascii(c: AgentClient):
    """Packet payload echo, plain ASCII."""
    data = b"The quick brown fox jumps over the lazy Victor 9000."
    out = c.command(CMD_ECHO, data)
    check(out == data, f"echo mismatch: {out!r}")
    return f"{len(data)} bytes"


@test('echo_binary')
def t_echo_binary(c: AgentClient):
    """Packet escaping round trip: SYNC/ESC/XON/XOFF and binary data."""
    data = bytes([PKT_SYNC, PKT_ESC, XON, XOFF, 0x00, 0xFF, 0xA5]) \
        + bytes(range(0, 256, 3))
    out = c.command(CMD_ECHO, data)
    check(out == data, "binary echo mismatch")
    return f"{len(data)} bytes incl. all escaped values"


@test('echo_large')
def t_echo_large(c: AgentClient):
    """Maximum-size payload echo."""
    n = PKT_MAX_PAYLOAD - 2     # response carries 2 header bytes
    data = bytes((i * 3) & 0xFF for i in range(n))
    out = c.command(CMD_ECHO, data, timeout=c.t(15.0))
    check(out == data, "large echo mismatch")
    return f"{n} bytes"


@test('crc16', covers=['pkt_crc16', 'pkt_crc16_byte'])
def t_crc16(c: AgentClient):
    """pkt_crc16()/pkt_crc16_byte() against the host implementation."""
    blobs = [b'', b'\x00', b'123456789', bytes(range(256))]
    for blob in blobs:
        d = c.command(CMD_CRC, blob)
        crc_block, crc_bytes = struct.unpack('<HH', d)
        expected = PacketProtocol.crc16(blob)
        check(crc_block == expected,
              f"pkt_crc16({blob[:8]!r}...)=0x{crc_block:04X}, "
              f"host says 0x{expected:04X}")
        check(crc_bytes == expected,
              f"pkt_crc16_byte chain=0x{crc_bytes:04X}, "
              f"expected 0x{expected:04X}")
    # Sanity-check the reference itself (CRC-16/CCITT-FALSE check value)
    check(PacketProtocol.crc16(b'123456789') == 0x29B1,
          "host CRC reference broken")
    return f"{len(blobs)} blobs verified (incl. empty)"


@test('status_idle',
      covers=['ser_get_baud', 'ser_get_error', 'ser_check_cts',
              'ser_check_dcd', 'ser_tx_ready', 'ser_rx_ready',
              'ser_int_rx_available', 'ser_int_tx_free'])
def t_status_idle(c: AgentClient):
    """Status snapshot in the idle state."""
    d = c.command(CMD_STATUS)
    baud, err, cts, dcd, txr, rxr = d[:6]
    rxav, txfree = struct.unpack('<HH', d[6:10])
    check(baud == c.start_baud_idx,
          f"ser_get_baud()={baud}, expected {c.start_baud_idx}")
    check(err == 0, f"ser_get_error()=0x{err:02X}, expected 0")
    check(txr == 1, "ser_tx_ready() not true while idle")
    check(rxr == 0, "ser_rx_ready() true while line idle")
    check(rxav == 0, f"rx_available={rxav} while idle")
    check(txfree == SER_BUF_SIZE,
          f"tx_free={txfree}, expected {SER_BUF_SIZE}")
    return f"CTS={cts} DCD={dcd} (informational on PTY)"


@test('raw_echo', covers=['ser_int_read', 'ser_int_write'])
def t_raw_echo(c: AgentClient):
    """Per-byte ser_int_read()/ser_int_write() echo, full binary range."""
    data = bytes(range(256))
    c.send_cmd(CMD_RAW_ECHO, struct.pack('<H', len(data)))
    c.conn.send(data)
    back = c.read_exact(len(data), c.t(10.0))
    check(back == data, "raw echo data mismatch")
    status, d = c.recv_rsp(CMD_RAW_ECHO, c.t(8.0))
    check(status == 0, f"raw echo status {status}")
    count = struct.unpack('<H', d)[0]
    check(count == len(data), f"agent echoed {count}/{len(data)}")
    return "256 bytes, all values 0x00-0xFF"


@test('write_str', covers=['ser_write_str'])
def t_write_str(c: AgentClient):
    """ser_write_str(): raw string arrives intact, count is right."""
    s = b"Hello from ser_write_str() on the Victor 9000!\r\n"
    c.send_cmd(CMD_WRITE_STR, s)
    raw = c.read_exact(len(s), c.t(8.0))
    check(raw == s, f"string mismatch: {raw!r}")
    status, d = c.recv_rsp(CMD_WRITE_STR, c.t(8.0))
    check(status == 0, f"status {status}")
    count = struct.unpack('<H', d)[0]
    check(count == len(s), f"return count {count}, expected {len(s)}")
    return f"{len(s)} bytes"


@test('write_block',
      covers=['ser_int_write_block', 'ser_int_tx_free',
              'ser_int_drain_tx'])
def t_write_block(c: AgentClient):
    """ser_int_write_block() + tx_free during/after drain_tx()."""
    n, seed = 512, 3
    c.send_cmd(CMD_WRITE_BLOCK, struct.pack('<HB', n, seed))
    raw = c.read_exact(n, c.t(10.0))
    check(raw == pat_bytes(seed, n), "pattern mismatch")
    status, d = c.recv_rsp(CMD_WRITE_BLOCK, c.t(10.0))
    check(status == 0, f"status {status}")
    written, free_during, free_after = struct.unpack('<HHH', d)
    check(written == n, f"written {written}/{n}")
    check(free_during < SER_BUF_SIZE,
          f"tx_free={free_during} right after queueing {n} bytes "
          "(nothing pending?)")
    check(free_during >= SER_BUF_SIZE - n,
          f"tx_free={free_during} below floor {SER_BUF_SIZE - n}")
    check(free_after == SER_BUF_SIZE,
          f"tx_free={free_after} after drain_tx, "
          f"expected {SER_BUF_SIZE}")
    return f"{n} bytes; tx_free {free_during} during, {free_after} after"


@test('read_block', covers=['ser_int_read_block'])
def t_read_block(c: AgentClient):
    """ser_int_read_block() collects a raw block."""
    n, seed = 400, 11
    data = pat_bytes(seed, n)
    c.send_cmd(CMD_READ_BLOCK, struct.pack('<H', n))
    c.conn.send(data)
    status, d = c.recv_rsp(CMD_READ_BLOCK, c.t(15.0))
    check(status == 0, f"status {status}")
    total = struct.unpack('<H', d[:2])[0]
    check(total == n, f"agent read {total}/{n}")
    check(d[2:] == data, "block data mismatch")
    return f"{n} bytes"


@test('flush_rx',
      covers=['ser_int_flush_rx', 'ser_int_rx_available'])
def t_flush_rx(c: AgentClient):
    """ser_int_flush_rx() empties a populated RX buffer."""
    n = 128
    c.send_cmd(CMD_RX_FLUSH, struct.pack('<HB', n, 0))
    c.conn.send(b'j' * n)
    status, d = c.recv_rsp(CMD_RX_FLUSH, c.t(15.0))
    check(status == 0, "agent timed out waiting for junk bytes")
    before, after = struct.unpack('<HH', d)
    check(before == n, f"rx_available={before} before flush, expected {n}")
    check(after == 0, f"rx_available={after} after flush")
    return f"{before} -> {after}"


@test('pkt_flush', covers=['pkt_flush'])
def t_pkt_flush(c: AgentClient):
    """pkt_flush() drains pending serial data."""
    n = 128
    c.send_cmd(CMD_RX_FLUSH, struct.pack('<HB', n, 1))
    c.conn.send(b'k' * n)
    status, d = c.recv_rsp(CMD_RX_FLUSH, c.t(15.0))
    check(status == 0, "agent timed out waiting for junk bytes")
    before, after = struct.unpack('<HH', d)
    check(before == n and after == 0,
          f"pkt_flush: {before} -> {after}, expected {n} -> 0")
    return f"{before} -> {after}"


@test('flush_tx', covers=['ser_int_flush_tx'])
def t_flush_tx(c: AgentClient):
    """ser_int_flush_tx() discards queued TX data (only bytes already
    handed to the transmitter escape)."""
    n, seed = 1024, 5
    c.send_cmd(CMD_FLUSH_TX, struct.pack('<HB', n, seed))
    stragglers = c.read_until_sync(c.t(10.0))
    status, d = c.recv_rsp(CMD_FLUSH_TX, c.t(8.0))
    check(status == 0, f"status {status}")
    queued = struct.unpack('<H', d)[0]
    check(queued == n, f"queued {queued}/{n}")
    check(len(stragglers) < 64,
          f"{len(stragglers)} bytes escaped the flush (of {n} queued)")
    check(stragglers == pat_bytes(seed, n)[:len(stragglers)],
          "straggler bytes are not the pattern prefix")
    return f"{n} queued, {len(stragglers)} escaped"


@test('rx_overflow', covers=['ser_get_error', 'ser_int_rx_available'])
def t_rx_overflow(c: AgentClient):
    """RX buffer overflow sets SER_ERR_BUFOVFL; ser_get_error() clears."""
    flood = SER_BUF_SIZE + 552
    c.send_cmd(CMD_OVERFLOW)
    c.conn.send(b'U' * flood)
    status, d = c.recv_rsp(CMD_OVERFLOW, c.t(30.0))
    check(status == 0, f"status {status}")
    max_avail = struct.unpack('<H', d[:2])[0]
    err, err_after = d[2], d[3]
    check(max_avail == SER_BUF_SIZE,
          f"buffer filled to {max_avail}, expected {SER_BUF_SIZE}")
    check(err & SER_ERR_BUFOVFL,
          f"ser_get_error()=0x{err:02X}, SER_ERR_BUFOVFL not set")
    check(err_after == 0,
          f"error flags not cleared by read: 0x{err_after:02X}")
    return f"filled {max_avail}, err=0x{err:02X} then cleared"


@test('flow_xonoff',
      covers=['ser_set_flow', 'ser_int_tx_free', 'ser_int_drain_tx'])
def t_flow_xonoff(c: AgentClient):
    """XOFF suspends TX (nothing leaks), XON resumes; the XOFF byte is
    consumed by the ISR, not delivered as data."""
    n, seed = 256, 7
    c.send_cmd(CMD_FLOW_XONOFF)
    status, _ = c.recv_rsp(CMD_FLOW_XONOFF, c.t(8.0))   # "armed"
    check(status == 0, "arming failed")

    c.conn.send(bytes([XOFF]))
    # The agent observes the suspension event and queues its data
    # within this window; a long quiet check is also a stronger test
    quiet = c.conn.receive_available(timeout=c.t(8.0))
    check(quiet == b'',
          f"{len(quiet)} bytes transmitted while suspended: "
          f"{quiet[:16]!r}")
    c.conn.send(bytes([XON]))

    raw = c.read_exact(n, c.t(20.0))
    check(raw == pat_bytes(seed, n), "resumed data mismatch")
    status, d = c.recv_rsp(CMD_FLOW_XONOFF, c.t(15.0))
    check(status == 0, "agent never observed the TX suspension "
                       "(XOFF not consumed by the ISR?)")
    observed = d[0]
    queued, free_during, rxav = struct.unpack('<HHH', d[1:7])
    check(observed == 1, "suspension flag not set")
    check(queued == n, f"queued {queued}/{n}")
    check(free_during <= SER_BUF_SIZE - 128,
          f"tx_free={free_during} while suspended; data was not held back")
    check(rxav == 0,
          f"rx_available={rxav}: XOFF leaked into the data stream")
    return f"{n} bytes held (tx_free={free_during}), then released by XON"


@test('flow_rtscts_smoke', covers=['ser_set_flow', 'ser_check_cts'])
def t_flow_rtscts(c: AgentClient):
    """ser_set_flow(RTSCTS) accepted and restored (PTY can't drop CTS)."""
    d = c.command(CMD_FLOW_SMOKE, bytes([1]))
    check(d[0] == 1, "ser_set_flow(SER_FLOW_RTSCTS) returned false")
    return f"accepted; CTS={d[1]} (informational)"


@test('baud_invalid', covers=['ser_set_baud', 'ser_get_baud'])
def t_baud_invalid(c: AgentClient):
    """ser_set_baud() rejects an out-of-range index."""
    d = c.command(CMD_BAUD_BAD, bytes([8]))
    check(d[0] == 0, "ser_set_baud(port, 8) did not return false")
    check(d[1] == c.start_baud_idx,
          f"baud changed to index {d[1]} by a rejected call")
    return "index 8 rejected, rate unchanged"


@test('baud_cycle', covers=['ser_set_baud', 'ser_get_baud'])
def t_baud_cycle(c: AgentClient):
    """ser_set_baud() to another rate and back; channel survives."""
    idx = BAUD_IDX[4800]
    c.send_cmd(CMD_BAUD_CYCLE, bytes([idx]))
    status, _ = c.recv_rsp(CMD_BAUD_CYCLE, c.t(8.0))
    check(status == 0, "phase-1 status nonzero")
    status, d = c.recv_rsp(CMD_BAUD_CYCLE, c.t(40.0))
    check(status == 0, f"phase-2 status {status}")
    r1, during, r2, after, err = d[:5]
    check(r1 == 1, "ser_set_baud(4800) returned false")
    check(during == idx, f"ser_get_baud()={during} during, expected {idx}")
    check(r2 == 1, "restoring baud returned false")
    check(after == c.start_baud_idx,
          f"ser_get_baud()={after} after restore")
    return f"9600 -> 4800 -> 9600 ok (err=0x{err:02X} during window)"


@test('format_invalid', covers=['ser_set_format'])
def t_format_invalid(c: AgentClient):
    """ser_set_format() rejects invalid data-bit counts."""
    for bad in (4, 9):
        d = c.command(CMD_FORMAT_BAD, bytes([bad, 1, 0]))
        check(d[0] == 0,
              f"ser_set_format(data_bits={bad}) did not return false")
    return "data_bits 4 and 9 rejected"


@test('format_cycle', covers=['ser_set_format'])
def t_format_cycle(c: AgentClient):
    """ser_set_format(7E1) applied and restored to 8N1; channel survives."""
    c.send_cmd(CMD_FORMAT_CYCLE, bytes([7, 1, 2]))
    status, _ = c.recv_rsp(CMD_FORMAT_CYCLE, c.t(8.0))
    check(status == 0, "phase-1 status nonzero")
    status, d = c.recv_rsp(CMD_FORMAT_CYCLE, c.t(40.0))
    check(status == 0, f"phase-2 status {status}")
    r1, r2, err = d[:3]
    check(r1 == 1, "ser_set_format(7,1,EVEN) returned false")
    check(r2 == 1, "restore to 8N1 returned false")
    return f"8N1 -> 7E1 -> 8N1 ok (err=0x{err:02X} during window)"


@test('init_port',
      covers=['ser_init_port', 'ser_int_enable', 'ser_int_disable'])
def t_init_port(c: AgentClient):
    """ser_init_port() valid reconfig (plus an interrupt-mode
    disable/enable cycle) and invalid-config rejection."""
    cfg = bytes([0, c.start_baud_idx, 8, 1, 0, 0])
    d = c.command(CMD_INIT_PORT, cfg, timeout=c.t(12.0))
    check(d[0] == 1, "ser_init_port(valid config) returned false")
    c.command(CMD_PING)     # channel must still work after the cycle
    d = c.command(CMD_INIT_PORT, bytes([5, 6, 8, 1, 0, 0]))
    check(d[0] == 0, "ser_init_port(port=5) did not return false")
    d = c.command(CMD_INIT_PORT, bytes([0, 9, 8, 1, 0, 0]))
    check(d[0] == 0, "ser_init_port(baud=9) did not return false")
    return "valid config + int-mode cycle ok; bad port/baud rejected"


@test('modem_lines',
      covers=['ser_set_rts', 'ser_set_dtr', 'ser_check_cts',
              'ser_check_dcd', 'ser_tx_ready', 'ser_rx_ready'])
def t_modem_lines(c: AgentClient):
    """RTS/DTR toggle plus status-bit sanity (line states are
    informational on a PTY)."""
    d = c.command(CMD_LINES, timeout=c.t(10.0))
    cts0, dcd0, cts1, dcd1, txr, rxr = d[:6]
    check(txr == 1, "ser_tx_ready() false while idle")
    check(rxr == 0, "ser_rx_ready() true while idle")
    return (f"CTS {cts0}->{cts1}, DCD {dcd0}->{dcd1} across "
            "RTS/DTR drop (informational)")


@test('rx_ready_window',
      covers=['ser_rx_ready', 'ser_int_disable', 'ser_int_enable'])
def t_rx_ready(c: AgentClient):
    """ser_rx_ready() observes a pending byte with interrupt mode off."""
    c.send_cmd(CMD_RX_READY)
    time.sleep(c.t(0.4))
    c.conn.send(b'R')
    status, d = c.recv_rsp(CMD_RX_READY, c.t(30.0))
    check(status == 0, f"status {status}")
    check(d[0] == 1, "ser_rx_ready() never went true for a pending byte")
    return "hardware RX bit observed during ISR-off window"


@test('pkt_params', covers=['pkt_set_timeout', 'pkt_set_retries'])
def t_pkt_params(c: AgentClient):
    """pkt_set_timeout()/pkt_set_retries() take effect."""
    d = c.command(CMD_PKT_PARAMS, struct.pack('<HB', 20000, 5))
    timeout, retries = struct.unpack('<HB', d[:3])
    check(timeout == 20000, f"timeout readback {timeout}")
    check(retries == 5, f"retries readback {retries}")
    # Channel still healthy with defaults restored
    out = c.command(CMD_ECHO, b'after-params')
    check(out == b'after-params', "echo after param restore failed")
    return "timeout/retries set, echoed back, defaults restored"


@test('pkt_crc_nak',
      covers=['pkt_send_nak', 'pkt_get_error', 'pkt_clear_error'])
def t_pkt_crc_nak(c: AgentClient):
    """Corrupted CRC draws a NAK; pkt_get_error()/pkt_clear_error()."""
    seq = c.proto._seq_tx
    pkt = build_raw_packet(
        PKT_TYPE_DATA | (PKT_SEQ_BIT if seq else 0),
        bytes([CMD_PING]), corrupt_crc=True)
    c.conn.send(pkt)
    t, _ = c.proto.recv_raw(c.t(6.0))
    check(t is not None, "no response to corrupt packet")
    check(t & PKT_TYPE_MASK == PKT_TYPE_NAK,
          f"expected NAK, got type 0x{t:02X}")
    s = c.stats()
    check(s['last_rc'] == -2,
          f"agent recorded rc {s['last_rc']}, expected -2 (PKT_ERR_CRC)")
    check(s['pkt_err'] == 2,
          f"pkt_get_error()={s['pkt_err']}, expected 2")
    s = c.stats()
    check(s['pkt_err'] == 0, "pkt_clear_error() did not clear")
    return "NAK received, error recorded then cleared"


@test('pkt_oversize')
def t_pkt_oversize(c: AgentClient):
    """Length field beyond PKT_MAX_PAYLOAD is rejected with a NAK."""
    c.conn.send(build_raw_packet(0, b'', length_override=1100,
                                 truncate_after_length=True))
    t, _ = c.proto.recv_raw(c.t(6.0))
    check(t is not None and t & PKT_TYPE_MASK == PKT_TYPE_NAK,
          "no NAK for oversize length")
    s = c.stats()
    check(s['last_rc'] == -4,
          f"agent recorded rc {s['last_rc']}, expected -4 "
          "(PKT_ERR_OVERFLOW)")
    return "length 1100 NAKed"


@test('pkt_mid_timeout')
def t_pkt_mid_timeout(c: AgentClient):
    """SYNC with no body times out mid-packet and draws a NAK."""
    c.conn.send(bytes([PKT_SYNC]))
    t, _ = c.proto.recv_raw(c.t(10.0))
    check(t is not None and t & PKT_TYPE_MASK == PKT_TYPE_NAK,
          "no NAK for mid-packet timeout")
    return "bare SYNC NAKed after receive timeout"


@test('pkt_stray_ack')
def t_pkt_stray_ack(c: AgentClient):
    """Unsolicited ACK is rejected as a frame error (no NAK storm)."""
    c.conn.send(build_raw_packet(PKT_TYPE_ACK))
    time.sleep(c.t(1.0))
    leak = c.drain(0.2)
    check(leak == b'', f"agent responded to a stray ACK: {leak[:16]!r}")
    s = c.stats()
    check(s['last_rc'] == -3,
          f"agent recorded rc {s['last_rc']}, expected -3 "
          "(PKT_ERR_FRAME)")
    return "stray ACK ignored, PKT_ERR_FRAME recorded"


@test('pkt_duplicate', covers=['pkt_receive', 'pkt_send_ack'])
def t_pkt_duplicate(c: AgentClient):
    """A retransmitted DATA packet (same sequence bit) is re-ACKed but
    not delivered twice."""
    before = c.stats()

    seq = c.proto._seq_tx
    pkt = build_raw_packet(PKT_TYPE_DATA | (PKT_SEQ_BIT if seq else 0),
                           bytes([CMD_PING]))
    c.conn.send(pkt)
    t, _ = c.proto.recv_raw(c.t(5.0))
    check(t is not None and t & PKT_TYPE_MASK == PKT_TYPE_ACK,
          "first transmission not ACKed")
    c.proto.receive_packet(timeout=c.t(6.0))    # the ping response

    c.conn.send(pkt)    # identical duplicate
    t, _ = c.proto.recv_raw(c.t(5.0))
    check(t is not None and t & PKT_TYPE_MASK == PKT_TYPE_ACK,
          "duplicate not re-ACKed")
    t, _ = c.proto.recv_raw(c.t(2.0))
    check(t is None, f"duplicate produced a second response (0x{t or 0:02X})")

    c.proto._seq_tx = seq ^ 1   # one delivery happened
    after = c.stats()
    check(after['pings'] - before['pings'] == 1,
          f"ping dispatched {after['pings'] - before['pings']} times, "
          "expected exactly 1")
    return "duplicate re-ACKed, dispatched once"


@test('pkt_retransmit', covers=['pkt_send'])
def t_pkt_retransmit(c: AgentClient):
    """Withholding the ACK makes the agent's pkt_send() retransmit the
    same packet with the same sequence bit."""
    seq_tx = c.proto._seq_tx
    seq_rx = c.proto._seq_rx
    c.conn.send(build_raw_packet(
        PKT_TYPE_DATA | (PKT_SEQ_BIT if seq_tx else 0),
        bytes([CMD_ECHO]) + b'RETRY-ME'))
    t, _ = c.proto.recv_raw(c.t(5.0))
    check(t is not None and t & PKT_TYPE_MASK == PKT_TYPE_ACK,
          "command not ACKed")

    t1, p1 = c.proto.recv_raw(c.t(6.0))     # response - do NOT ack
    check(t1 is not None and t1 & PKT_TYPE_MASK == PKT_TYPE_DATA,
          "no response received")
    t2, p2 = c.proto.recv_raw(c.t(8.0))     # the retransmission
    check(t2 is not None, "agent never retransmitted an un-ACKed packet")
    check(t2 == t1 and p2 == p1,
          "retransmission differs from the original")

    ack_seq = 1 if (t1 & PKT_SEQ_BIT) else 0
    c.conn.send(build_raw_packet(
        PKT_TYPE_ACK | (PKT_SEQ_BIT if ack_seq else 0)))
    c.proto._seq_tx = seq_tx ^ 1
    c.proto._seq_rx = seq_rx ^ 1
    time.sleep(c.t(0.3))
    c.drain(0.2)    # in case a third transmission was already queued
    c.command(CMD_PING)
    return "retransmission observed, late ACK accepted"


@test('wait_sync_junk', covers=['pkt_wait_sync'])
def t_wait_sync_junk(c: AgentClient):
    """Leading non-SYNC garbage is skipped before a valid packet."""
    c.conn.send(b'\x55' * 64)
    d = c.command(CMD_PING, timeout=c.t(10.0))
    check(len(d) >= 6, "ping through junk failed")
    return "64 junk bytes skipped"


@test('shutdown', covers=['ser_shutdown'], needs_quit=True)
def t_shutdown(c: AgentClient):
    """CMD_QUIT: agent acknowledges, then ser_shutdown()s and exits."""
    c.command(CMD_QUIT)
    return "agent shut down"


# ---------------------------------------------------------------------------
# Fake agent for --selftest (validates the harness without MAME)
# ---------------------------------------------------------------------------

class FdConnection(Connection):
    """Connection over a raw file descriptor (PTY master)."""

    def __init__(self, fd: int, **kw):
        super().__init__(**kw)
        self._fd = fd

    def connect(self) -> bool:
        self._connected = True
        self.start_receiver()
        return True

    def disconnect(self) -> None:
        self._running = False
        self._connected = False

    def _raw_send(self, data: bytes) -> int:
        return os.write(self._fd, data)

    def _raw_receive(self, count: int) -> bytes:
        try:
            return os.read(self._fd, count)
        except (BlockingIOError, OSError):
            return b''

    @property
    def bytes_available(self) -> int:
        import fcntl
        import termios
        try:
            buf = fcntl.ioctl(self._fd, termios.FIONREAD, b'\x00' * 4)
            return struct.unpack('i', buf)[0]
        except OSError:
            return 0


class FakeAgent(threading.Thread):
    """Python stand-in for sertest.exe, speaking the identical wire
    protocol with behaviorally-equivalent responses. Validates the host
    harness and protocol plumbing; it does NOT validate the C library."""

    START_BAUD_IDX = BAUD_IDX[9600]

    def __init__(self, master_fd: int):
        super().__init__(daemon=True)
        self.conn = FdConnection(master_fd)
        self.conn.connect()
        self.proto = PacketProtocol(self.conn, timeout=1.0)
        self.seq_rx = 0
        self.cmd_count = 0
        self.ping_count = 0
        self.last_rc = 0
        self.pkt_err = 0
        self.running = True
        self.error = None

    def run(self):
        try:
            while self.running:
                payload = self._recv_command()
                if payload is None:
                    continue
                self.cmd_count += 1
                if self._dispatch(payload):
                    break
        except Exception:
            self.error = traceback.format_exc()
        finally:
            self.conn.disconnect()

    # --- protocol receive mirroring the C agent main loop ---

    def _recv_command(self):
        t, p = self.proto.recv_raw(timeout=0.2)
        if t is None:
            err = self.proto.last_error or ''
            if 'CRC mismatch' in err:
                self.last_rc, self.pkt_err = -2, 2
                self.proto.send_nak()
            elif 'too large' in err:
                self.last_rc, self.pkt_err = -4, 4
                self.proto.send_nak()
            elif 'waiting for sync' in err:
                pass    # idle
            else:
                self.pkt_err = 1            # mid-packet timeout
                self.proto.send_nak()
            return None

        base = t & PKT_TYPE_MASK
        seq = 1 if (t & PKT_SEQ_BIT) else 0
        if base != PKT_TYPE_DATA:
            self.last_rc, self.pkt_err = -3, 3      # PKT_ERR_FRAME
            return None

        self.proto.send_ack(seq)
        if seq != self.seq_rx:
            return None     # duplicate: re-ACKed above, not delivered
        self.seq_rx ^= 1
        return p

    def _rsp(self, op, status=0, data=b''):
        # 1s ACK timeout mirrors the C agent's PKT_TIMEOUT_LONG, so the
        # retransmit test sees the same pacing as the real agent
        self.proto.send_packet(bytes([op, status]) + data, timeout=1.0)

    # --- dispatch ---

    def _dispatch(self, p: bytes) -> bool:
        op, args = p[0], p[1:]

        if op == CMD_PING:
            self.ping_count += 1
            self._rsp(op, 0, bytes([AGENT_VER_MAJOR, 0])
                      + struct.pack('<HH', self.cmd_count, self.ping_count))
        elif op == CMD_ECHO:
            self._rsp(op, 0, args)
        elif op == CMD_CRC:
            crc = PacketProtocol.crc16(args)
            self._rsp(op, 0, struct.pack('<HH', crc, crc))
        elif op == CMD_STATUS:
            self._rsp(op, 0, bytes([self.START_BAUD_IDX, 0, 1, 1, 1, 0])
                      + struct.pack('<HH', 0, SER_BUF_SIZE))
        elif op == CMD_RAW_ECHO:
            n = struct.unpack('<H', args[:2])[0]
            data = self.conn.receive(n, timeout=8.0)
            self.conn.send(data)
            self._rsp(op, 0 if len(data) == n else 1,
                      struct.pack('<H', len(data)))
        elif op == CMD_WRITE_STR:
            self.conn.send(args)
            self._rsp(op, 0, struct.pack('<H', len(args)))
        elif op == CMD_WRITE_BLOCK:
            n, seed = struct.unpack('<HB', args[:3])
            self.conn.send(pat_bytes(seed, n))
            self._rsp(op, 0, struct.pack('<HHH', n,
                                         SER_BUF_SIZE - n + 16,
                                         SER_BUF_SIZE))
        elif op == CMD_READ_BLOCK:
            n = struct.unpack('<H', args[:2])[0]
            data = self.conn.receive(n, timeout=8.0)
            self._rsp(op, 0 if len(data) == n else 1,
                      struct.pack('<H', len(data)) + data)
        elif op == CMD_RX_FLUSH:
            n, _mode = struct.unpack('<HB', args[:3])
            data = self.conn.receive(n, timeout=8.0)
            self._rsp(op, 0 if len(data) == n else 1,
                      struct.pack('<HH', len(data), 0))
        elif op == CMD_OVERFLOW:
            total = 0
            while True:
                chunk = self.conn.receive_available(timeout=1.0)
                if not chunk:
                    break
                total += len(chunk)
            self._rsp(op, 0, struct.pack('<H', min(total, SER_BUF_SIZE))
                      + bytes([SER_ERR_BUFOVFL if total > SER_BUF_SIZE
                               else 0, 0]))
        elif op == CMD_FLUSH_TX:
            n, seed = struct.unpack('<HB', args[:3])
            self.conn.send(pat_bytes(seed, 2))      # the "stragglers"
            self._rsp(op, 0, struct.pack('<H', n))
        elif op == CMD_FLOW_XONOFF:
            self._rsp(op, 0)    # armed
            self._wait_for_byte(XOFF, 15.0)
            self._wait_for_byte(XON, 20.0)
            self.conn.send(pat_bytes(7, 256))
            self._rsp(op, 0, bytes([1])
                      + struct.pack('<HHH', 256,
                                    SER_BUF_SIZE - 256 + 16, 0))
        elif op == CMD_FLOW_SMOKE:
            self._rsp(op, 0, bytes([1, 1]))
        elif op == CMD_BAUD_BAD:
            ret = 1 if args[0] < 8 else 0
            self._rsp(op, 0, bytes([ret, self.START_BAUD_IDX]))
        elif op == CMD_BAUD_CYCLE:
            self._rsp(op, 0)
            time.sleep(0.2)
            self._rsp(op, 0, bytes([1, args[0], 1, self.START_BAUD_IDX, 0]))
        elif op == CMD_FORMAT_BAD:
            ret = 1 if 5 <= args[0] <= 8 else 0
            self._rsp(op, 0, bytes([ret]))
        elif op == CMD_FORMAT_CYCLE:
            self._rsp(op, 0)
            time.sleep(0.2)
            self._rsp(op, 0, bytes([1, 1, 0]))
        elif op == CMD_INIT_PORT:
            ret = 1 if (args[0] <= 1 and args[1] < 8) else 0
            self._rsp(op, 0, bytes([ret]))
        elif op == CMD_LINES:
            self._rsp(op, 0, bytes([1, 1, 1, 1, 1, 0]))
        elif op == CMD_RX_READY:
            b = self.conn.receive(1, timeout=8.0)
            self._rsp(op, 0, bytes([1 if b else 0, 1]))
        elif op == CMD_PKT_PARAMS:
            self._rsp(op, 0, args[:3])
        elif op == CMD_HWSTATE:
            b = self.conn.receive(1, timeout=8.0)   # the probe byte
            self._rsp(op, 0, bytes([1, 0xFD, 0, 0, 0x04, 0x04])
                      + struct.pack('<HH', 1 if b else 0, 0))
        elif op == CMD_STATS:
            rc = self.last_rc & 0xFFFF
            self._rsp(op, 0, struct.pack('<HHH', self.cmd_count,
                                         self.ping_count, rc)
                      + bytes([self.pkt_err]) + struct.pack('<H', 0))
            self.last_rc = 0
            self.pkt_err = 0
        elif op == CMD_QUIT:
            self._rsp(op, 0)
            self.running = False
            return True
        else:
            self._rsp(op, 2)
        return False

    def _wait_for_byte(self, value: int, timeout: float):
        deadline = time.time() + timeout
        while time.time() < deadline:
            b = self.conn.receive(1, timeout=0.2)
            if b and b[0] == value:
                return
        raise TimeoutError(f"fake agent: byte 0x{value:02X} never arrived")


# ---------------------------------------------------------------------------
# Line probe (hardware bring-up triage)
# ---------------------------------------------------------------------------

def run_probe(conn) -> int:
    """Wire-level triage for real hardware. Stateless on the agent side:
    the deliberately-corrupt probe packet never touches the sequence
    bits, so the suite can run immediately after a successful probe."""
    print("--- Serial line probe ---")

    ser = getattr(conn, '_serial', None)
    if ser is not None:
        try:
            print(f"Modem lines: CTS={ser.cts} DSR={ser.dsr} "
                  f"CD={ser.cd} RI={ser.ri}")
        except OSError:
            print("Modem lines: not readable (normal for a PTY)")
        try:
            ser.dtr = True
            ser.rts = True
            print("DTR and RTS asserted.")
        except OSError:
            pass

    print("Listening 3s for stray traffic "
          "(restart sertest.exe now to catch its banner)...")
    stray = conn.receive_available(timeout=3.0)
    if stray:
        print(f"  {len(stray)} bytes: {stray[:64].hex(' ')}")
        printable = stray[:64].decode('latin-1')
        print(f"  as text: {printable!r}")
        if b'SERTEST AGENT' in stray:
            print("  -> Agent banner seen: Victor->PC direction works.")
    else:
        print("  (line quiet)")

    print("Sending a deliberately corrupt packet (agent should NAK)...")
    conn.send(build_raw_packet(PKT_TYPE_DATA, bytes([CMD_PING]),
                               corrupt_crc=True))
    reply = conn.receive(8, timeout=6.0)
    if reply[:4] == bytes([PKT_SYNC, 0, 0, PKT_TYPE_NAK]):
        print(f"NAK received ({reply.hex(' ')}).")
        print("Agent is alive and BOTH directions work - run the suite.")
        return 0
    if reply:
        print(f"Reply is not a NAK: {reply.hex(' ')}")
        print("Likely a baud mismatch or line noise; check the rate "
              "sertest was started with.")
        return 1

    print("No reply. Triage:")
    print("  - Victor screen shows 'rx err rc=02': PC->Victor works, "
          "Victor->PC is broken (check the Victor TX -> PC RX wire).")
    print("  - Victor screen shows nothing: PC->Victor is broken "
          "(null-modem crossover? right port? right baud?).")
    print("  - No banner at sertest startup either: Victor TX path or "
          "cable is dead, or sertest.exe is an old build.")
    return 1


# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------

def coverage_report(results, ran_quit):
    covered = set()
    for entry in SUITE:
        if any(r['name'] == entry['name'] and r['status'] == 'PASS'
               for r in results):
            covered.update(entry['covers'])

    lines = []
    for header, funcs in (('serial.h', SERIAL_H_FUNCS),
                          ('packet.h', PACKET_H_FUNCS)):
        missing = [f for f in funcs if f not in covered]
        if not ran_quit and 'ser_shutdown' in missing:
            missing.remove('ser_shutdown')
            lines.append(f"  {header}: {len(funcs) - len(missing) - 1}"
                         f"/{len(funcs)} verified "
                         "(ser_shutdown needs --quit)")
        else:
            lines.append(f"  {header}: {len(funcs) - len(missing)}"
                         f"/{len(funcs)} verified")
        if missing:
            lines.append(f"    not verified: {', '.join(missing)}")
    return lines


def run_suite(client, selected, run_quit, verbose):
    results = []
    width = max(len(e['name']) for e in SUITE) + 2
    aborted = False

    for i, entry in enumerate(selected):
        name = entry['name']
        label = f"[{i + 1:2}/{len(selected)}] {name} "
        print(label + '.' * (width - len(name)), end=' ', flush=True)

        if aborted:
            print("SKIP  (agent unreachable)")
            results.append({'name': name, 'status': 'SKIP',
                            'detail': 'agent unreachable'})
            continue

        start = time.time()
        try:
            detail = entry['fn'](client) or ''
            status = 'PASS'
        except TestSkip as e:
            status, detail = 'SKIP', str(e)
        except TestFailure as e:
            status, detail = 'FAIL', str(e)
        except (PacketError, Exception) as e:
            status = 'FAIL'
            detail = f"{type(e).__name__}: {e}"
            if verbose:
                traceback.print_exc()

        elapsed = time.time() - start
        print(f"{status}  {detail}" + (f"  [{elapsed:.1f}s]"
                                       if verbose else ""))
        results.append({'name': name, 'status': status, 'detail': detail})

        if status == 'FAIL' and entry is not selected[-1]:
            if not client.resync():
                aborted = True

        client.drain(0.05)

    return results


def main():
    parser = argparse.ArgumentParser(
        description='Automated test suite for the Victor 9000 serial '
                    'library (drives sertest.exe)',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s                       # auto-detect MAME PTY, run everything
  %(prog)s --port /dev/pts/5     # explicit port
  %(prog)s --selftest            # validate harness against a fake agent
  %(prog)s --only crc16,echo_ascii
  %(prog)s --quit                # also test ser_shutdown (agent exits)
        """)
    parser.add_argument('--port', '-p', default=None,
                        help='PTY or serial port (default: auto-detect '
                             'MAME PTY)')
    parser.add_argument('--baud', '-b', type=int, default=9600,
                        help='Baud rate the agent was started with '
                             '(default: 9600)')
    parser.add_argument('--selftest', action='store_true',
                        help='Run against a local fake agent (no MAME); '
                             'validates the harness, not the C library')
    parser.add_argument('--probe', action='store_true',
                        help='Wire-level triage for real hardware: report '
                             'modem lines, listen for the agent banner, '
                             'expect a NAK to a corrupt packet. Run this '
                             'first when bringing up a physical link.')
    parser.add_argument('--list', action='store_true',
                        help='List tests and coverage, then exit')
    parser.add_argument('--only', default=None,
                        help='Comma-separated test names to run')
    parser.add_argument('--quit', action='store_true',
                        help='Send CMD_QUIT at the end (covers '
                             'ser_shutdown; agent must be restarted '
                             'before the next run)')
    parser.add_argument('--time-scale', type=float, default=None,
                        help='Scale factor for agent-paced waits '
                             '(default 1.0, selftest 0.25)')
    parser.add_argument('--verbose', '-v', action='store_true')
    args = parser.parse_args()

    if args.list:
        print(f"{len(SUITE)} tests:")
        for e in SUITE:
            covers = f"  [{', '.join(e['covers'])}]" if e['covers'] else ''
            print(f"  {e['name']:20} {e['doc'].strip()}{covers}")
        return 0

    selected = [e for e in SUITE
                if (args.quit or not e['needs_quit'])]
    if args.only:
        wanted = {w.strip() for w in args.only.split(',')}
        unknown = wanted - {e['name'] for e in SUITE}
        if unknown:
            print(f"Unknown test(s): {', '.join(sorted(unknown))}")
            return 2
        selected = [e for e in selected if e['name'] in wanted]

    fake = None
    if args.selftest:
        if args.baud != 9600:
            print("Note: --selftest always reports 9600 baud")
            args.baud = 9600
        import tty
        master, slave = os.openpty()
        tty.setraw(slave)
        fake = FakeAgent(master)
        fake.start()
        conn = SerialConnection(port=os.ttyname(slave), baudrate=9600)
        mode = 'selftest (fake agent)'
        scale = args.time_scale if args.time_scale is not None else 0.25
    else:
        if args.port:
            conn = SerialConnection(port=args.port, baudrate=args.baud)
            mode = 'serial port'
        else:
            conn = MamePtyConnection(baudrate=args.baud)
            mode = 'MAME PTY'
        scale = args.time_scale if args.time_scale is not None else 1.0

    if args.baud not in BAUD_IDX:
        print(f"Unsupported baud rate {args.baud}")
        return 2

    try:
        conn.connect()
    except Exception as e:
        print(f"Connection failed: {e}")
        return 2
    conn.start_receiver()

    if args.probe:
        try:
            return run_probe(conn)
        finally:
            conn.disconnect()

    client = AgentClient(conn, scale=scale)
    client.start_baud_idx = BAUD_IDX[args.baud]

    print("=" * 64)
    print("Victor 9000 Serial Library Test Suite")
    print(f"Port: {getattr(conn, 'port', '?')}   Baud: {args.baud}   "
          f"Mode: {mode}")
    print("=" * 64)

    start = time.time()
    kick_note = None
    try:
        results = run_suite(client, selected, args.quit, args.verbose)
        if not args.quit:
            try:
                kicks = client.stats()['kicks']
                if kicks:
                    kick_note = (
                        f"WARNING: agent needed {kicks} lost-edge "
                        "interrupt recoveries (ser_int_kick). Data "
                        "flowed, but the SIO->8259A interrupt chain "
                        "is dropping edges on this hardware.")
            except (TestFailure, PacketError):
                pass
    finally:
        conn.disconnect()
        if fake is not None:
            fake.running = False
            fake.join(timeout=3.0)

    elapsed = time.time() - start
    passed = sum(1 for r in results if r['status'] == 'PASS')
    failed = sum(1 for r in results if r['status'] == 'FAIL')
    skipped = sum(1 for r in results if r['status'] == 'SKIP')

    print("=" * 64)
    print(f"{len(results)} tests: {passed} passed, {failed} failed, "
          f"{skipped} skipped   ({elapsed:.1f}s)")
    if kick_note:
        print(kick_note)
    for line in coverage_report(results, args.quit):
        print(line)
    if args.selftest:
        print("NOTE: selftest validates the harness and wire protocol "
              "only, not the C library.")
        if fake is not None and fake.error:
            print("Fake agent error:\n" + fake.error)
            return 1
    print("=" * 64)

    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
