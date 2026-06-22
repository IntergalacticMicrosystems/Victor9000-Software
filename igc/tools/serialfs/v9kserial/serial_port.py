"""
Physical Serial Port Connection.

Provides connection via physical RS-232 serial port using pyserial.
"""

import time
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

    def __init__(self, port: str, baudrate: int = 9600, timeout: float = 1.0,
                 pace: bool = False,
                 byte_delay: float = 0.0, cts_gate: bool = False,
                 cts_timeout: float = 0.5, gate_settle: float = 0.003):
        """
        Initialize serial connection.

        Args:
            port: Serial port path (e.g., '/dev/ttyUSB0', 'COM1')
            baudrate: Baud rate
            timeout: Read timeout in seconds
            pace: Write one byte per write()+flush() instead of the whole frame
                  at once, so the Victor's slow polled receiver and 3-byte FIFO
                  can keep up at higher line rates without relying on the USB
                  adapter's (too-laggy) hardware flow control.
            byte_delay: Seconds to sleep after each paced byte (only used when
                  pace is set). 0 = back-to-back single-byte writes.
            cts_gate: Software, packet-granular flow control. Before writing a
                  frame, sleep gate_settle and (best-effort) wait for CTS. The
                  Victor is 3-wire and holds its RTS (our CTS) asserted the
                  whole time, so the CTS check returns immediately and gives no
                  "ready" edge; the actual throttle is the gate_settle delay,
                  which brackets the Victor's brief non-polling window at packet
                  boundaries where ms-scale USB latency is harmless.
            cts_timeout: Max seconds to wait for CTS before sending anyway
                  (best-effort fallback so a stuck line can't deadlock; the
                  packet layer's own retry/timeout then covers it). With this
                  3-wire Victor, CTS is always asserted, so this rarely elapses.
            gate_settle: Seconds to sleep before each gated frame. This is the
                  real gating knob: it covers the request->response turnaround,
                  where the Victor has a brief non-polling window between sending
                  its request and re-entering its receive loop. A few ms lets it
                  get there.
        """
        super().__init__(baudrate, timeout)
        self.port = port
        self.pace = pace
        self.byte_delay = byte_delay
        self.cts_gate = cts_gate
        self.cts_timeout = cts_timeout
        self.gate_settle = gate_settle
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
                rtscts=False,   # 3-wire link; hardware flow control is inert here
                dsrdtr=False
            )
            # Assert DTR and RTS explicitly: peers (or UARTs in auto-enable modes)
            # may gate their receiver/transmitter on these lines. We hold both
            # asserted statically since the link carries no hardware flow control.
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

    def _wait_cts(self) -> None:
        """Block until CTS is asserted (peer ready), up to cts_timeout.

        Note: the Victor is 3-wire and holds its RTS (our CTS) asserted the
        whole time, so in practice CTS is already high and this returns at once
        - the real between-packet throttle is the gate_settle sleep in _raw_send.
        Kept best-effort for any peer that does drop CTS: on timeout we send
        anyway and let the packet layer's retry recover.
        """
        deadline = time.time() + self.cts_timeout
        while time.time() < deadline:
            try:
                if self._serial.cts:
                    return
            except (OSError, serial.SerialException):
                return  # no modem line (PTY) - don't gate
            time.sleep(0.0005)

    def _raw_send(self, data: bytes) -> int:
        """Send data to serial port.

        In paced mode each byte is written and flushed individually (with an
        optional inter-byte delay) so a slow polled receiver can keep up; see
        the `pace` / `byte_delay` constructor args. In cts_gate mode the whole
        frame is held until the peer asserts CTS (packet-granular flow control).
        """
        if not (self._serial and self._serial.is_open):
            return 0
        if self.cts_gate:
            if self.gate_settle:
                time.sleep(self.gate_settle)  # cover the turnaround gap
            self._wait_cts()                  # cover the between-packet gap
        if self.pace:
            mv = memoryview(data)
            for i in range(len(mv)):
                self._serial.write(mv[i:i + 1])
                self._serial.flush()  # drain OS buffer to the chip per byte
                if self.byte_delay:
                    time.sleep(self.byte_delay)
            return len(data)
        count = self._serial.write(data)
        self._serial.flush()  # Ensure data is actually sent
        return count

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
