"""
Base Connection Class for Serial Communications.

Provides abstract interface for serial connections with
background receive thread and queue-based data handling.
"""

from abc import ABC, abstractmethod
from typing import Optional
import threading
from queue import Queue, Empty
import time


class ConnectionError(Exception):
    """Serial connection error."""
    pass


class Connection(ABC):
    """
    Abstract base class for serial connections.

    Provides common functionality for serial communication including
    background receive thread and send/receive methods.

    Subclasses must implement:
        - connect()
        - disconnect()
        - _raw_send()
        - _raw_receive()
        - bytes_available (property)
    """

    def __init__(self, baudrate: int = 9600, timeout: float = 1.0):
        """
        Initialize connection.

        Args:
            baudrate: Serial baud rate
            timeout: Default read timeout in seconds
        """
        self.baudrate = baudrate
        self.timeout = timeout
        self._connected = False
        self._rx_queue: Queue = Queue()
        self._rx_thread: Optional[threading.Thread] = None
        self._running = False
        self._lock = threading.Lock()
        self._rx_buffer = b''  # Buffer for excess bytes from receive()
        self._rx_error: Optional[Exception] = None  # Fatal receive-thread error

    @property
    def is_connected(self) -> bool:
        """Check if connection is active."""
        return self._connected

    @property
    def rx_error(self) -> Optional[Exception]:
        """Exception that killed the receive thread, if any."""
        return self._rx_error

    @abstractmethod
    def connect(self) -> bool:
        """
        Establish connection.

        Returns:
            True on success

        Raises:
            ConnectionError: On failure
        """
        pass

    @abstractmethod
    def disconnect(self) -> None:
        """Close connection and cleanup."""
        pass

    @abstractmethod
    def _raw_send(self, data: bytes) -> int:
        """
        Low-level send (implemented by subclass).

        Args:
            data: Bytes to send

        Returns:
            Number of bytes sent
        """
        pass

    @abstractmethod
    def _raw_receive(self, count: int) -> bytes:
        """
        Low-level receive (implemented by subclass).

        Args:
            count: Maximum bytes to receive

        Returns:
            Received bytes (may be empty)
        """
        pass

    @property
    @abstractmethod
    def bytes_available(self) -> int:
        """Number of bytes available to read."""
        pass

    def send(self, data: bytes) -> int:
        """
        Send data.

        Args:
            data: Bytes to send

        Returns:
            Number of bytes sent

        Raises:
            ConnectionError: If not connected
        """
        if not self._connected:
            raise ConnectionError("Not connected")
        return self._raw_send(data)

    def receive(self, count: int = 1, timeout: Optional[float] = None) -> bytes:
        """
        Receive exactly count bytes (or fewer on timeout).

        Args:
            count: Number of bytes to receive
            timeout: Timeout in seconds (uses default if None)

        Returns:
            Received bytes (exactly count bytes, or fewer on timeout)
        """
        if timeout is None:
            timeout = self.timeout

        # Start with any buffered data from previous receive
        with self._lock:
            result = self._rx_buffer
            self._rx_buffer = b''

        deadline = time.time() + timeout

        while len(result) < count:
            remaining = deadline - time.time()
            if remaining <= 0:
                break

            try:
                data = self._rx_queue.get(timeout=min(remaining, 0.1))
                result += data
            except Empty:
                pass

        # Return exactly count bytes, buffer excess for next receive
        if len(result) > count:
            with self._lock:
                self._rx_buffer = result[count:] + self._rx_buffer
            result = result[:count]

        return result

    def receive_available(self, timeout: float = 0.01) -> bytes:
        """
        Receive all available data.

        Args:
            timeout: Short timeout to wait for data

        Returns:
            All available bytes
        """
        # Start with buffered data
        with self._lock:
            result = self._rx_buffer
            self._rx_buffer = b''

        try:
            while True:
                data = self._rx_queue.get(timeout=timeout)
                result += data
        except Empty:
            pass
        return result

    def receive_until(self, terminator: bytes, timeout: Optional[float] = None,
                      max_bytes: int = 4096) -> bytes:
        """
        Receive until terminator is found.

        Args:
            terminator: Byte sequence to wait for
            timeout: Timeout in seconds
            max_bytes: Maximum bytes to receive

        Returns:
            Received bytes including terminator
        """
        if timeout is None:
            timeout = self.timeout

        # Start with buffered data
        with self._lock:
            result = self._rx_buffer
            self._rx_buffer = b''

        deadline = time.time() + timeout

        while len(result) < max_bytes:
            # Check if terminator already in buffered data
            if terminator in result:
                idx = result.find(terminator) + len(terminator)
                # Buffer any excess data
                if idx < len(result):
                    with self._lock:
                        self._rx_buffer = result[idx:] + self._rx_buffer
                return result[:idx]

            remaining = deadline - time.time()
            if remaining <= 0:
                break

            try:
                data = self._rx_queue.get(timeout=min(remaining, 0.1))
                result += data
            except Empty:
                pass

        return result

    def unread(self, data: bytes) -> None:
        """
        Push bytes back so the next receive returns them first.

        Useful when a reader has to scan past raw data up to a framing
        byte that belongs to the next protocol layer.
        """
        with self._lock:
            self._rx_buffer = data + self._rx_buffer

    def flush_rx(self) -> None:
        """Clear receive buffer."""
        with self._lock:
            self._rx_buffer = b''
        while not self._rx_queue.empty():
            try:
                self._rx_queue.get_nowait()
            except Empty:
                break

    def start_receiver(self) -> None:
        """Start background receive thread."""
        if self._rx_thread is not None and self._rx_thread.is_alive():
            return

        self._running = True
        self._rx_thread = threading.Thread(target=self._rx_worker, daemon=True)
        self._rx_thread.start()

    def stop_receiver(self) -> None:
        """Stop background receive thread."""
        self._running = False
        if self._rx_thread:
            self._rx_thread.join(timeout=2.0)
            self._rx_thread = None

    def _rx_worker(self) -> None:
        """Background thread to receive data."""
        while self._running and self._connected:
            try:
                if self.bytes_available > 0:
                    data = self._raw_receive(min(256, self.bytes_available))
                    if data:
                        self._rx_queue.put(data)
                else:
                    time.sleep(0.01)
            except Exception as e:
                # A dead receive thread would otherwise look like a healthy
                # connection that never receives anything: record the error
                # and mark the connection failed so callers find out.
                if self._running:
                    self._rx_error = e
                    self._connected = False
                break
