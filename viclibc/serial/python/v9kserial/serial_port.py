"""
Physical Serial Port Connection.

Provides connection via physical RS-232 serial port using pyserial.
"""

from typing import Optional

try:
    import serial
except ImportError:
    raise ImportError("pyserial not installed. Run: pip install pyserial")

from .connection import Connection, ConnectionError


class SerialConnection(Connection):
    """
    Connection via physical serial port.

    Uses pyserial for cross-platform serial port access.

    Example:
        conn = SerialConnection('/dev/ttyUSB0', baudrate=9600)
        conn.connect()
        conn.send(b"Hello")
        data = conn.receive()
        conn.disconnect()
    """

    def __init__(self, port: str, baudrate: int = 9600, timeout: float = 1.0):
        """
        Initialize serial connection.

        Args:
            port: Serial port path (e.g., '/dev/ttyUSB0', 'COM1')
            baudrate: Baud rate
            timeout: Read timeout in seconds
        """
        super().__init__(baudrate, timeout)
        self.port = port
        self._serial: Optional[serial.Serial] = None

    def connect(self) -> bool:
        """Open connection to serial port."""
        try:
            self._serial = serial.Serial(
                port=self.port,
                baudrate=self.baudrate,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=0.01,  # Short timeout for non-blocking
                xonxoff=False,
                rtscts=False,
                dsrdtr=False
            )
            # Assert DTR/RTS explicitly: peers (or UARTs in auto-enable
            # modes) may gate their receiver/transmitter on these lines.
            try:
                self._serial.dtr = True
                self._serial.rts = True
            except (OSError, serial.SerialException):
                pass    # PTYs have no modem lines
            self._connected = True
            self.start_receiver()
            return True
        except serial.SerialException as e:
            raise ConnectionError(f"Failed to open {self.port}: {e}")

    def disconnect(self) -> None:
        """Close serial connection."""
        self.stop_receiver()
        if self._serial and self._serial.is_open:
            self._serial.close()
        self._connected = False
        self._serial = None

    def _raw_send(self, data: bytes) -> int:
        """Send data to serial port."""
        if self._serial and self._serial.is_open:
            count = self._serial.write(data)
            self._serial.flush()  # Ensure data is actually sent
            return count
        return 0

    def _raw_receive(self, count: int) -> bytes:
        """Receive data from serial port."""
        if self._serial and self._serial.is_open:
            available = self._serial.in_waiting
            if available > 0:
                return self._serial.read(min(count, available))
        return b''

    @property
    def bytes_available(self) -> int:
        """Number of bytes available to read."""
        if self._serial and self._serial.is_open:
            return self._serial.in_waiting
        return 0

    def set_rts(self, state: bool) -> None:
        """Set RTS line state."""
        if self._serial:
            self._serial.rts = state

    def set_dtr(self, state: bool) -> None:
        """Set DTR line state."""
        if self._serial:
            self._serial.dtr = state

    @property
    def cts(self) -> bool:
        """Get CTS line state."""
        if self._serial:
            try:
                return self._serial.cts
            except (OSError, serial.SerialException):
                return False
        return False

    @property
    def dsr(self) -> bool:
        """Get DSR line state."""
        if self._serial:
            try:
                return self._serial.dsr
            except (OSError, serial.SerialException):
                return False
        return False

    @property
    def dcd(self) -> bool:
        """Get DCD (Carrier Detect) line state."""
        if self._serial:
            try:
                return self._serial.cd
            except (OSError, serial.SerialException):
                return False
        return False

    @property
    def ri(self) -> bool:
        """Get RI (Ring Indicator) line state."""
        if self._serial:
            try:
                return self._serial.ri
            except (OSError, serial.SerialException):
                return False
        return False

    def send_break(self, duration: float = 0.25) -> None:
        """Send a break condition."""
        if self._serial:
            self._serial.send_break(duration)

    def flush(self) -> None:
        """Flush output buffer (wait for all data to be sent)."""
        if self._serial:
            self._serial.flush()

    def reset_input_buffer(self) -> None:
        """Clear input buffer."""
        if self._serial:
            self._serial.reset_input_buffer()
        self.flush_rx()

    def reset_output_buffer(self) -> None:
        """Clear output buffer."""
        if self._serial:
            self._serial.reset_output_buffer()
