# v9kserial - Victor 9000 Serial Communications

Cross-platform Python module for serial communications with the Victor 9000.
Supports physical serial ports and MAME PTY connections.

## Installation

```bash
pip install pyserial
pip install .
```

## Quick Start

### Connect via MAME PTY (Auto-detect)

```python
from v9kserial import MamePtyConnection

# Start MAME first: ./mame victor9k -rs232a pty

conn = MamePtyConnection(baudrate=9600)
conn.connect()

conn.send(b"Hello Victor!\r\n")
response = conn.receive_available()
print(f"Received: {response}")

conn.disconnect()
```

### Connect via Physical Serial Port

```python
from v9kserial import SerialConnection

conn = SerialConnection('/dev/ttyUSB0', baudrate=9600)
conn.connect()

conn.send(b"Hello Victor!\r\n")
response = conn.receive(10, timeout=2.0)

conn.disconnect()
```

### Use Packet Protocol for Reliable Transfer

```python
from v9kserial import MamePtyConnection, PacketProtocol

conn = MamePtyConnection(baudrate=9600)
conn.connect()

pkt = PacketProtocol(conn)

# Send packet with ACK/NAK
pkt.send_packet(b"Reliable data")

# Receive packet
response = pkt.receive_packet(timeout=5.0)
print(f"Received: {response}")

conn.disconnect()
```

## API Reference

### Connection Classes

- `SerialConnection(port, baudrate, timeout)` - Physical serial port
- `MamePtyConnection(baudrate, timeout, pty_path)` - MAME PTY auto-detect

### Connection Methods

- `connect()` - Open connection
- `disconnect()` - Close connection
- `send(data)` - Send bytes
- `receive(count, timeout)` - Receive bytes
- `receive_available()` - Receive all available data
- `flush_rx()` - Clear receive buffer

### PacketProtocol Methods

- `send_packet(data)` - Send data with ACK
- `receive_packet(timeout)` - Receive data with ACK
- `send_ack()` / `send_nak()` - Send control packets

## Packet Format

```
+------+--------+------+---------+------+
| SYNC | LENGTH | TYPE | PAYLOAD | CRC  |
+------+--------+------+---------+------+
   1       2       1     0-2000    2
```

- SYNC: 0x7E (after SYNC, the bytes 0x7E/0x7D/XON/XOFF are escaped as
  0x7D followed by the byte XOR 0x20)
- LENGTH: Little-endian, max 2000 (`PKT_MAX_PAYLOAD`)
- TYPE: bits 0-6: DATA=0x01, ACK=0x02, NAK=0x03, RESET=0x04; bit 7:
  alternating sequence bit on DATA/ACK (suppresses duplicate delivery
  after a lost ACK). RESET resyncs the sequence bits on (re)connect -
  `PacketProtocol.reset()`.
- CRC: CRC-16-CCITT over TYPE+PAYLOAD

## License

MIT License
