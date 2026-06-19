"""
Victor 9000 Serial Communications Library (Modern Side)

Cross-platform Python module for serial communications with the Victor 9000.
Supports physical serial ports and MAME PTY connections.

Usage:
    from v9kserial import SerialConnection, MamePtyConnection, PacketProtocol

    # Connect via MAME PTY (auto-detect)
    conn = MamePtyConnection(baudrate=9600)
    conn.connect()
    conn.send(b"Hello Victor!")
    data = conn.receive(timeout=1.0)
    conn.disconnect()

    # Use packet protocol for reliable transfer
    pkt = PacketProtocol(conn)
    pkt.send_packet(b"Reliable data")
    response = pkt.receive_packet()
"""

from .connection import Connection, ConnectionError
from .serial_port import SerialConnection
from .mame_pty import MamePtyConnection
from .packet import PacketProtocol, PacketError
from .constants import *

__version__ = '1.0.0'
__author__ = 'Victor 9000 Development'

__all__ = [
    # Classes
    'Connection',
    'ConnectionError',
    'SerialConnection',
    'MamePtyConnection',
    'PacketProtocol',
    'PacketError',
    # Constants
    'BAUD_110', 'BAUD_300', 'BAUD_600', 'BAUD_1200',
    'BAUD_2400', 'BAUD_4800', 'BAUD_9600', 'BAUD_19200',
    'PKT_SYNC', 'PKT_TYPE_DATA', 'PKT_TYPE_ACK', 'PKT_TYPE_NAK',
]
