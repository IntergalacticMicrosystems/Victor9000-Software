# Victor 9000 Serial Communications Library

A robust serial communications library for data transfer between
the 8086-based Victor 9000 and modern machines. **Polled I/O only** (no
interrupts, no ring buffers): a tight 8088 poll loop drains the SIO 7201's
3-byte RX FIFO fast enough to stay clean to 38400 (the async ceiling), 4× the
old interrupt path's reliable 9600. Packet framing on top gives reliable
transfers with CRC-16 and ACK/NAK retry.

## Project Structure

```
serial/
├── include/
│   ├── serial.h          # Core serial API (interrupt + polled primitives)
│   └── packet.h          # Packet protocol API (pkt_init / pkt_init_polled)
├── src/
│   ├── serial.c          # Main implementation (config, buffers, ISR, polled I/O)
│   ├── serial_hw.c       # Hardware access layer (7201/8253/6522/8259 MMIO)
│   └── packet.c          # Packet protocol (framing, CRC, pkt_recv_raw hot loop)
├── test/
│   ├── pkttest.c         # PKTTEST.EXE - packet receive hot-loop profiler
│   └── spdtest.c         # SPDTEST.EXE - raw polled throughput test
├── python/
│   ├── v9kserial/        # Python package
│   ├── setup.py
│   └── requirements.txt
├── pkttest_host.py       # Host side of PKTTEST
├── spdtest_host.py       # Host side of SPDTEST
├── run_spd.sh            # Orchestrates a SPDTEST run via remote control
├── Makefile              # OpenWatcom build (default: spdtest.exe; `wmake pkt`)
└── AGENTS.md             # This file
```

---

## Victor 9000 C Library

### Building

Requires OpenWatcom v2 for 8086 target:

```bash
wmake           # Build library and test program
wmake lib       # Build library only
wmake clean     # Clean build artifacts
```

### Quick Start

```c
#include "serial.h"

void main(void) {
    /* Initialize serial subsystem (both ports 8N1, 1200 baud) */
    ser_init();

    /* Set baud rate (polled I/O - no interrupt enable step) */
    ser_set_baud(SER_PORT_A, SER_BAUD_9600);

    /* Send a string, one polled byte at a time */
    const char *msg = "Hello from Victor!\r\n";
    while (*msg) ser_poll_write(SER_PORT_A, (uint8_t)*msg++);

    /* Receive and echo data */
    while (1) {
        int16_t ch = ser_poll_read(SER_PORT_A);   /* -1 if no byte ready */
        if (ch >= 0) {
            ser_poll_write(SER_PORT_A, (uint8_t)ch);
        }
    }

    ser_shutdown();
}
```

### Packet Protocol

```c
#include "serial.h"
#include "packet.h"

void main(void) {
    pkt_state_t pkt;
    uint8_t buf[256];
    int16_t len;

    ser_init();
    ser_set_baud(SER_PORT_A, SER_BAUD_9600);

    /* Initialize packet layer */
    pkt_init(&pkt, SER_PORT_A);

    /* Send packet with ACK */
    pkt_send(&pkt, (uint8_t *)"Hello", 5);

    /* Receive packet */
    len = pkt_receive(&pkt, buf, sizeof(buf));
    if (len > 0) {
        /* Process received data */
    }

    ser_shutdown();
}
```

### API Reference

#### Initialization
- `ser_init()` - Initialize both ports (8N1, 1200 baud)
- `ser_init_port(config)` - Initialize with configuration
- `ser_shutdown()` - Shutdown and cleanup

#### Configuration
- `ser_set_baud(port, baud_idx)` - Set baud rate
- `ser_set_format(port, data, stop, parity)` - Set data format
- `ser_set_flow(port, mode)` - Set flow control

#### Hardware Status
- `ser_rx_ready(port)` - Check if RX data available in hardware
- `ser_tx_ready(port)` - Check if TX register ready

#### Polled I/O (no interrupts)
- `ser_poll_read(port)` - Non-blocking read straight from the SIO; byte 0..255, or -1 if none ready
- `ser_poll_write(port, data)` - Blocking send of one byte (spin until TX buffer empty)
- `ser_poll_drain(port)` - Wait until the transmitter is fully drained ("All Sent"); use before turning the line around

#### Status / Control
- `ser_get_error(port)` - Get and clear accumulated error flags (`SER_ERR_*`)
- `ser_check_dcd(port)` - Data Carrier Detect status
- `ser_set_dtr(port, active)` - Assert/deassert DTR
- `ser_get_baud(port)` - Current baud index

#### Constants
```c
/* Ports */
SER_PORT_A, SER_PORT_B

/* Baud rates (38400 = async ceiling; 76800 = PIT divisor 1, hardware max) */
SER_BAUD_110, SER_BAUD_300, SER_BAUD_600, SER_BAUD_1200
SER_BAUD_2400, SER_BAUD_4800, SER_BAUD_9600, SER_BAUD_19200
SER_BAUD_38400, SER_BAUD_76800

/* Data format */
SER_DATA_5..8, SER_STOP_1/15(=1.5)/2, SER_PARITY_NONE/ODD/EVEN

/* Flow control (no hardware RTS/CTS - 3-wire link) */
SER_FLOW_NONE, SER_FLOW_XONOFF
```

---

## Python Module (Modern Side)

### Installation

```bash
cd python
pip install pyserial
pip install .
```

### MAME PTY Connection

```python
from v9kserial import MamePtyConnection

# Start MAME: ./mame victor9k -rs232a pty

conn = MamePtyConnection(baudrate=9600)
conn.connect()

conn.send(b"Hello Victor!\r\n")
response = conn.receive_available()

conn.disconnect()
```

### Physical Serial Port

```python
from v9kserial import SerialConnection

conn = SerialConnection('/dev/ttyUSB0', baudrate=9600)
conn.connect()

conn.send(b"Hello Victor!\r\n")
response = conn.receive(10, timeout=2.0)

conn.disconnect()
```

### Packet Protocol

```python
from v9kserial import MamePtyConnection, PacketProtocol

conn = MamePtyConnection(baudrate=9600)
conn.connect()

pkt = PacketProtocol(conn)
pkt.send_packet(b"Reliable data")
response = pkt.receive_packet(timeout=5.0)

conn.disconnect()
```

---

## Hardware Reference

### Victor 9000 Serial Hardware

| Component | Segment | Purpose |
|-----------|---------|---------|
| SIO 7201 | E004h | Dual-channel serial controller |
| PIT 8253 | E002h | Baud rate generator |
| VIA 6522 | E800h | Clock selection |
| PIC 8259A | E000h | Interrupt controller (present, but unused - library is polled) |

- **Baud Clock**: 2.4576 MHz crystal, x16 mode. 38400 = PIT divisor 2 (async
  ceiling); 76800 (divisor 1) is the hardware max but invalid for async mode 3.
- The SIO interrupt line is left masked; only the SIO's own interrupt is unused,
  so the BIOS 18.2 Hz timer tick still runs (e.g. for `ftx_get_ticks`).

### Packet Format

```
+------+--------+------+---------+------+
| SYNC | LENGTH | TYPE | PAYLOAD | CRC  |
+------+--------+------+---------+------+
   1       2       1     0-2000    2

SYNC:   0x7E. After the SYNC byte, 0x7E, 0x7D, XON (0x11) and
        XOFF (0x13) are escaped as 0x7D followed by the byte XOR 0x20
LENGTH: 16-bit little-endian
TYPE:   bits 0-6: DATA=0x01, ACK=0x02, NAK=0x03, RESET=0x04
        bit 7:    alternating sequence bit (DATA and ACK) - lets the
                  receiver detect and re-ACK (but not re-deliver)
                  retransmissions caused by a lost ACK
CRC:    CRC-16-CCITT (poly 0x1021, init 0xFFFF) over TYPE+PAYLOAD
```

**RESET (sequence-bit resync / connection hello).** Sequence bits live in
`pkt_state_t` for the life of the connection, so a peer that reconnects with
fresh state (seq 0) while the other side's `seq_rx` is odd would have its first
DATA packet mistaken for a duplicate (ACKed but not delivered). The initiator
calls `pkt_send_reset()` (C) / `PacketProtocol.reset()` (Python) right after
(re)connecting: it zeroes the local sequence bits and sends an empty RESET
packet. The responder handles RESET transparently inside `pkt_receive()` -
zeroes its own `seq_rx`/`seq_tx` and ACKs - so no application change is needed.
Best-effort and backward compatible: a peer that predates RESET NAKs/ignores it.

---

## Testing

### In-tree Victor test programs

Two focused, hardware-in-the-loop tests build from `test/` in this tree, each
driven over a null-modem link from a host script:

- **`SPDTEST.EXE`** (`test/spdtest.c` + `spdtest_host.py`, orchestrated by
  `run_spd.sh`) — measures the raw **polled** serial throughput ceiling on Port B
  (COM2). See the "Polled transfer speed" section below for the full results.
- **`PKTTEST.EXE`** (`test/pkttest.c` + `pkttest_host.py`) — drives the real
  packet layer: `pkt_wait_sync()` + the `pkt_recv_raw()` inline-asm hot loop per
  frame, reporting the per-byte guard-wait min/max (built with
  `-DPKT_PROFILE_GUARD`). The host frames a known pattern with a short
  inter-frame gap so the between-frame CRC/copy can't overrun the 3-byte FIFO.

`wmake` builds `spdtest.exe` by default; `wmake pkt` builds `pkttest.exe`. Both
are run from DOS via the remote-control `type` command (each prints `R`/ready,
then receives N frames) and report recv/gap/overrun counts on the Victor console.

### Comprehensive function-coverage suite (reference)

`victor9k_serial_test.py` is the host side of an older command-driven suite that
exercised every public function of the **interrupt-driven** library with a
per-function coverage report. Its Victor agent (`sertest.c` → `sertest.exe`) is
**not present in this `viclibc2` tree**, and much of what it covered (interrupt
I/O, ring-buffer overflow, RTS/CTS, the `ser_int_*` API) no longer exists now
that the library is polled-only. The description below is kept as a record of
the behaviors that were validated on real hardware and the findings that drove
the move to polled mode; to actually run it you need the `sertest` agent from
the sibling `viclibc` tree.

#### What the suite covers

- **Data paths** - packet echo (incl. escaping of SYNC/ESC/XON/XOFF and
  max-size payloads), per-byte raw echo over the full 0x00-0xFF range,
  block read/write, `ser_write_str`
- **Buffers** - `rx_available`/`tx_free` accounting, RX/TX flush,
  `drain_tx`, RX overflow (`SER_ERR_BUFOVFL` set by the ISR and cleared
  by `ser_get_error`)
- **Flow control** - XON/XOFF suspension (XOFF holds queued TX data, XON
  releases it, and the XOFF byte itself is consumed by the ISR rather
  than delivered as data)
- **Configuration** - baud/format set+restore cycles (the line garbles
  against a fixed-rate peer, like real hardware, then recovers),
  parameter validation, `ser_init_port`, interrupt-mode disable/enable
- **Status/control** - CTS/DCD/RTS/DTR, `tx_ready`/`rx_ready`
  (including observing the hardware RX bit during an ISR-off window)
- **Packet protocol robustness** - CRC verification against the host,
  corrupt-CRC -> NAK, oversize-length -> NAK, mid-packet timeout -> NAK,
  stray ACK -> frame error, duplicate sequence bit -> re-ACKed but
  delivered once, withheld ACK -> retransmission

The wire protocol (opcodes and payload layouts) is defined in
`test/sertest.c` and mirrored in `victor9k_serial_test.py`; keep the two
in sync when adding commands.

#### With real hardware

Run the suite with an explicit port (`--port COM22` / `--port
/dev/ttyUSB0`). For link bring-up, probe first:

```bash
python3 victor9k_serial_test.py --port COM22 --probe
```

The probe reports modem-line states, listens for the agent's startup
banner (`SERTEST AGENT v2 READY` - restart sertest.exe while probing to
see it), and sends a deliberately corrupt packet that the agent must
NAK. A NAK proves both directions work; the probe output explains how
to triage each failure direction using the Victor's screen messages.

**History notes (June 2026)** - two real-hardware failures that MAME's
lenient emulation masked:

1. *Auto Enables*: the library used to set the SIO's Auto Enables bit
   (CR3 bit 5), which on real silicon gates RX on DCD and TX on CTS -
   a cable without those lines wired made the port completely deaf and
   mute. The bit is no longer set; flow control is purely software
   (`ser_set_flow`).

3. *SIO interrupt enables that only work in MAME*: with CR1 values
   0x17/0x13 and CR2A=0x10, a real 7201 never latches an interrupt at
   all (the `int_health` diagnostic showed RX data pending with the
   chip's own int-pending bit clear), while MAME interrupts fine. The
   only silicon-proven interrupt config we have is Victor Kermit's
   (msxv90.asm): **CR2A=0x14, CR1=0x18** - RX interrupts on all
   characters only. TX is therefore driven by the ser_int_write()
   prime path plus ser_int_pump() in the drain loops (every send path
   ends in ser_int_drain_tx), and RTS/CTS resume is pump-polled
   (ext/status interrupts are off). Also mirror the BIOS: dummy RR0
   reads before init to reset the SIO register pointer, and CR2B=0.

2. *Interrupt dispatch and lost edges*: the ISR used to identify the
   interrupt source from RR2B's status-affects-vector field, whose bit
   positions on a real 7201 depend on the CR2A mode (and an
   uninitialized WR2B vector base). One misdecoded source leaves its
   interrupt-pending latch set, the INT line never falls, and the
   edge-triggered 8259A never fires again (observed as TX dying after
   ~3 bytes). The ISR now dispatches purely from RR0/RR1 status bits,
   and `ser_int_kick()` (called from all wait loops) services stuck
   sources by polling, so a lost edge self-heals. The recovery count
   is reported in the agent's STATS response; the suite prints a
   warning when it is nonzero - data flows either way, but a climbing
   count means the interrupt chain is unhealthy.

#### Harness self-test (no MAME needed)

```bash
python3 victor9k_serial_test.py --selftest --quit
```

Runs the full suite against a Python fake agent on a local PTY speaking
the identical wire protocol. This validates the host harness and
protocol plumbing only - not the C library.

---

## Polled transfer speed (June 2026)

`test/spdtest.c` (-> `SPDTEST.EXE`) + `spdtest_host.py` measure the ceiling of
**pure-polled** raw serial (no ISR, PIC SIO line left masked) on **Port B =
COM2 = SER_PORT_B**, over the null-modem cable to the PC's `/dev/ttyUSB0`. The
tight loops read RR0 directly via MMIO (the library's CLI-wrapped accessors are
too slow per byte at high baud) and move one byte at a time. The host sources/
sinks an incrementing pattern and times wire throughput; the Victor reports
recv/gaps/overrun. `run_spd.sh <R|T|X> <baudidx> <count> [i]` orchestrates a run
(host listens, then types the DOS command via the remote control).

**Result: polled mode is CLEAN in BOTH directions at every rate up to the
hardware max (38400), with no size limit.** Hardware-verified:

| Dir | 9600 | 19200 | 38400 |
|-----|------|-------|-------|
| RX (PC->Victor) | CLEAN 967 B/s | CLEAN 1953 B/s | CLEAN ~3.8 KB/s |
| TX (Victor->PC) | CLEAN 977 B/s | CLEAN 1953 B/s | CLEAN ~3.9 KB/s |

All at line rate (baud/10), 0 gaps, 0 overruns. Verified sustained to 100 KB
each way at 38400 (32-bit counters; nothing is buffered, so size is unbounded).
This is the key contrast with the **interrupt-driven** path, which overruns the
3-byte FIFO above 9600 (see the 19200 note in `../victor9k_remote_control`): the
per-byte ISR entry/exit/EOI cost can't keep up, but a bare poll loop easily
can - even with the 18.2 Hz timer tick still running. The `i` flag (CLI around
the transfer) made **no difference** at these rates; disabling IRQs is not
needed. 38400 (PIT divisor 2, x16 clock) is the practical async ceiling - divisor
1 (76800) is invalid in PIT mode 3, and async needs the x16 sampling clock.

**uPD7201 RTS is software-only - CONFIRMED (mode `X`).** The probe asserts RTS,
announces ready, then deliberately stalls (polls RR0 without ever reading the
DATA register) so the RX FIFO overflows, never touching RTS in software; the
host floods bytes while sampling its CTS (= the Victor's RTS via the crossed
null modem). Flooding 200-1000 bytes into the non-reading receiver: exactly
**3 bytes** survived (confirming the 3-byte RX FIFO), RR1 showed **OVERRUN**
(0x23), and our CTS **stayed asserted the whole time**. So the 7201 does *not*
auto-deassert RTS when its FIFO fills - RTS is a plain CR5 output bit. Hardware
RTS/CTS therefore cannot raise the RX ceiling per-byte; only software watermark
throttling (`ser_set_rx_flow`) or - far simpler and faster here - polled drain
works. For max-speed bulk transfer in this range, no flow control is needed at
all.

> Gotcha for anyone extending `spdtest.c`: under `-ox` an empty busy-wait loop
> and a `(void)`-discarded volatile read both get optimized away. The stall must
> read a volatile (RR0) and drained bytes must be assigned to a live variable.
> Also, RR1 error flags track the byte at the TOP of the FIFO, so OR RR1 across
> the whole drain to catch an overrun tagged on the last byte.

**Polled packet layer + FTXSERV at 38400 (2026-06-18).** The polled primitives
above (`ser_poll_read`/`ser_poll_write`/`ser_poll_drain`) back a polled mode in
the packet layer (`pkt_init_polled`, `pkt_state_t.polled`) and FTXSERV, so file
transfer runs clean at **38400** - 4× the interrupt path's 9600 ceiling.
Hardware-verified: putfile + getfile of 64 KB and 256 KB byte-identical both
ways, `--rle`, listdir. Two findings were decisive:

1. **Per-byte MMIO budget.** Each access to the E004 SIO region carries wait
   states, so the polled RX hot path must be lean: `ser_poll_read` reads RR0 with
   a *single* bare load (relying on the register pointer staying at 0), not the
   library's reselect-then-read (2 MMIO). With the 2-MMIO version, the per-byte
   critical path exceeded one byte-time at 38400 and long packets slowly
   overran - small 6-byte LIST packets arrived fine but ~86-byte START packets
   failed every time. Cutting to one MMIO fixed it. (The raw `spdtest` loop was
   already this lean, which is why it sustained 38400 while the first packet port
   did not.)
2. **No RTS gating — the link is 3-wire.** Polled mode has no ISR ring buffer to
   absorb bytes during the receiver's between-packet disk write, and the uPD7201
   does not auto-deassert RTS when its FIFO fills (see the software-only-RTS
   finding above), so hardware RTS/CTS cannot throttle the peer per-byte. Instead
   the receiver simply keeps up: the lean single-MMIO `ser_poll_read` (finding 1)
   drains the 3-byte FIFO in well under a byte-time, and the packet protocol's
   ACK round-trip already brackets the between-packet window — the sender blocks
   for an ACK before sending the next packet, so it is paused while the receiver
   does its disk write. `pkt_receive`/`pkt_send` never touch RTS; CR5 holds RTS
   asserted statically from init.

Polled mode is now the **only** mode: the interrupt-driven path and its ring
buffers / `ser_int_*` API / `ser_set_rx_flow` watermark throttle were removed
once polled proved clean and faster (`pkt_init` and `pkt_init_polled` are now
identical aliases). The SIO's own interrupt is left masked, but global
interrupts stay ON, so `ftx_get_ticks` (BIOS 18.2 Hz timer) still works.

## Historical: DOS Interrupt Behavior (interrupt-driven era)

> The library is polled-only now, so the pitfall below no longer applies to it.
> Kept as a hard-won finding for anyone reintroducing interrupt-driven init.

**Critical Finding (December 2025):** DOS and BIOS functions (including `printf()`)
internally enable interrupts (STI) even when called with interrupts disabled.

### The Problem

When initializing interrupt-driven serial I/O:
1. ISR is installed
2. CPU interrupts disabled with `_disable()`
3. SIO chip configured to generate interrupts
4. PIC unmasked to pass interrupts to CPU
5. CPU interrupts re-enabled with `_enable()`

If `printf()` is called between steps 3-5, DOS internally does `STI`, which causes
the pending SIO interrupt to fire before initialization is complete, leading to
hangs on real hardware.

### Solution

**Never use DOS/BIOS functions (printf, INT 21h, INT 10h) inside critical sections
where interrupts must remain disabled.** The (former) `ser_int_enable()` used
only direct hardware I/O during initialization for this reason.

For debugging during critical sections, use direct video memory writes:
```c
/* Direct video memory write (doesn't use BIOS/DOS) */
volatile char __far *video = (char __far *)MK_FP(0xB000, 0);
video[0] = 'X';  /* Write character */
video[1] = 0x07; /* Normal attribute */
```

---

## Reference Files

- `/root/sync/Victor9000-Development-Private/asm86lib/ref/SERIAL.md` - Hardware documentation
- `/root/sync/Victor9000-Development-Private/asm86lib/layer1/l1_serial_hw.asm` - ASM reference
- `/root/sync/Victor9000-Development-Private/asm86lib/layer1/l1_serial_hw.inc` - Hardware constants
