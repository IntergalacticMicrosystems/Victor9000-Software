"""
Constants for Victor 9000 Serial Communications.

Mirrors the constants defined in the C library for consistency.
"""

# Baud rates (index values matching Victor side)
BAUD_110 = 0
BAUD_300 = 1
BAUD_600 = 2
BAUD_1200 = 3
BAUD_2400 = 4
BAUD_4800 = 5
BAUD_9600 = 6
BAUD_19200 = 7
BAUD_38400 = 8

# Baud rate values
BAUD_RATES = {
    BAUD_110: 110,
    BAUD_300: 300,
    BAUD_600: 600,
    BAUD_1200: 1200,
    BAUD_2400: 2400,
    BAUD_4800: 4800,
    BAUD_9600: 9600,
    BAUD_19200: 19200,
    BAUD_38400: 38400,
}

# Packet protocol constants
PKT_SYNC = 0x7E         # Start of packet
PKT_ESC = 0x7D          # Escape character
PKT_ESC_XOR = 0x20      # XOR for escaping

PKT_TYPE_DATA = 0x01    # Data packet
PKT_TYPE_ACK = 0x02     # Acknowledgment
PKT_TYPE_NAK = 0x03     # Negative acknowledgment
PKT_TYPE_RESET = 0x04   # Sequence-bit resync (connection hello)

PKT_SEQ_BIT = 0x80      # Bit 7 of TYPE: alternating sequence bit (DATA/ACK)
PKT_TYPE_MASK = 0x7F    # Bits 0-6 of TYPE: packet type

PKT_MAX_PAYLOAD = 2000  # Maximum payload size (match viclibc2 packet.h)

# Default timeouts (seconds)
DEFAULT_TIMEOUT = 1.0
DEFAULT_RETRIES = 3

# Flow control
FLOW_NONE = 0
FLOW_RTSCTS = 1
FLOW_XONOFF = 2

XON = 0x11   # DC1 (Ctrl-Q)
XOFF = 0x13  # DC3 (Ctrl-S)
