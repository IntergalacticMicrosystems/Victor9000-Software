"""
Error Injection for Serial Library Testing

Wraps the PacketProtocol to inject various errors for testing:
- Packet drops (simulates transmission failure)
- CRC corruption (triggers NAK/retry)
- Delayed responses (tests timeout handling)
- Sequence errors (tests out-of-order handling)

Usage:
    from v9kfiletrx.error_injector import ErrorInjector, ErrorConfig

    pkt = PacketProtocol(conn)
    injector = ErrorInjector(pkt, ErrorConfig(
        drop_probability=0.1,  # Drop 10% of packets
        corrupt_probability=0.05,  # Corrupt 5% of packets
    ))

    # Use injector like PacketProtocol
    ftx = FileTransfer(injector)
"""

import time
import random
from dataclasses import dataclass, field
from typing import Optional


@dataclass
class ErrorConfig:
    """Configuration for error injection."""
    # Probability-based errors (0.0 to 1.0)
    drop_probability: float = 0.0
    corrupt_probability: float = 0.0
    delay_probability: float = 0.0

    # Fixed packet number errors
    drop_packets: list = field(default_factory=list)  # List of packet numbers to drop
    corrupt_packets: list = field(default_factory=list)  # List to corrupt
    delay_packets: list = field(default_factory=list)  # List to delay

    # Delay settings
    delay_seconds: float = 2.0

    # Limits
    max_drops: int = 3  # Maximum packets to drop per transfer
    max_corrupts: int = 3  # Maximum packets to corrupt


@dataclass
class ErrorStats:
    """Statistics about injected errors."""
    packets_sent: int = 0
    packets_dropped: int = 0
    packets_corrupted: int = 0
    packets_delayed: int = 0
    packets_received: int = 0


class ErrorInjector:
    """
    Wraps PacketProtocol to inject errors for testing.

    Implements the same interface as PacketProtocol so it can be used
    as a drop-in replacement for testing error recovery.
    """

    def __init__(self, pkt_protocol, config: Optional[ErrorConfig] = None):
        """
        Initialize error injector.

        Args:
            pkt_protocol: The underlying PacketProtocol to wrap
            config: Error injection configuration
        """
        self.pkt = pkt_protocol
        self.config = config or ErrorConfig()
        self.stats = ErrorStats()
        self._enabled = True

    @property
    def conn(self):
        """Proxy to underlying connection."""
        return self.pkt.conn

    @property
    def timeout(self) -> float:
        """Proxy to underlying timeout."""
        return self.pkt.timeout

    @timeout.setter
    def timeout(self, value: float):
        """Set underlying timeout."""
        self.pkt.timeout = value

    @property
    def retries(self) -> int:
        """Proxy to underlying retries."""
        return self.pkt.retries

    @retries.setter
    def retries(self, value: int):
        """Set underlying retries."""
        self.pkt.retries = value

    @property
    def last_error(self) -> Optional[str]:
        """Proxy to underlying last_error."""
        return self.pkt.last_error

    def enable(self):
        """Enable error injection."""
        self._enabled = True

    def disable(self):
        """Disable error injection."""
        self._enabled = False

    def reset_stats(self):
        """Reset error statistics."""
        self.stats = ErrorStats()

    def _should_drop(self) -> bool:
        """Determine if this packet should be dropped."""
        if not self._enabled:
            return False

        # Check if this packet number is in drop list
        if self.stats.packets_sent in self.config.drop_packets:
            return True

        # Check probability
        if self.config.drop_probability > 0:
            if random.random() < self.config.drop_probability:
                if self.stats.packets_dropped < self.config.max_drops:
                    return True

        return False

    def _should_corrupt(self) -> bool:
        """Determine if this packet should be corrupted."""
        if not self._enabled:
            return False

        # Check if this packet number is in corrupt list
        if self.stats.packets_sent in self.config.corrupt_packets:
            return True

        # Check probability
        if self.config.corrupt_probability > 0:
            if random.random() < self.config.corrupt_probability:
                if self.stats.packets_corrupted < self.config.max_corrupts:
                    return True

        return False

    def _should_delay(self) -> bool:
        """Determine if response should be delayed."""
        if not self._enabled:
            return False

        # Check if this packet number is in delay list
        if self.stats.packets_received in self.config.delay_packets:
            return True

        # Check probability
        if self.config.delay_probability > 0:
            if random.random() < self.config.delay_probability:
                return True

        return False

    def _corrupt_data(self, data: bytes) -> bytes:
        """Corrupt data by flipping some bits."""
        if not data:
            return data

        # Corrupt the last 2 bytes (CRC area in most protocols)
        data_list = list(data)
        if len(data_list) >= 2:
            data_list[-1] ^= 0xFF
            data_list[-2] ^= 0xFF
        return bytes(data_list)

    def send_raw(self, pkt_type: int, payload: bytes = b'') -> bool:
        """Send a raw packet with potential error injection."""
        self.stats.packets_sent += 1

        if self._should_drop():
            self.stats.packets_dropped += 1
            print(f"[ErrorInjector] Dropped packet #{self.stats.packets_sent}")
            return True  # Pretend success, but don't actually send

        if self._should_corrupt():
            self.stats.packets_corrupted += 1
            payload = self._corrupt_data(payload)
            print(f"[ErrorInjector] Corrupted packet #{self.stats.packets_sent}")

        return self.pkt.send_raw(pkt_type, payload)

    def recv_raw(self, timeout: Optional[float] = None):
        """Receive a raw packet with potential delay injection."""
        self.stats.packets_received += 1

        if self._should_delay():
            self.stats.packets_delayed += 1
            delay = self.config.delay_seconds
            print(f"[ErrorInjector] Delaying receive by {delay}s")
            time.sleep(delay)

        return self.pkt.recv_raw(timeout)

    def send_packet(self, data: bytes, timeout: Optional[float] = None) -> bool:
        """
        Send data packet with acknowledgment (with error injection).

        Args:
            data: Payload data
            timeout: Timeout for ACK

        Returns:
            True on success

        Raises:
            PacketError: On failure
        """
        self.stats.packets_sent += 1

        drop = self._should_drop()
        corrupt = (not drop) and self._should_corrupt()
        if not (drop or corrupt):
            return self.pkt.send_packet(data, timeout)

        # Inject the fault into ONLY the first wire transmission, then let
        # PacketProtocol.send_packet's own ACK-timeout / NAK retransmit recover
        # it. Faulting the whole logical send (and faking success) would
        # silently skip a chunk and desync the sequence bits - that is not a
        # recoverable wire error, so it never exercises the retransmit path.
        n = self.stats.packets_sent
        orig_send_raw = self.pkt.send_raw
        fired = [False]

        def one_shot_send_raw(pkt_type, payload=b''):
            if fired[0]:
                return orig_send_raw(pkt_type, payload)
            fired[0] = True
            if drop:
                self.stats.packets_dropped += 1
                print(f"[ErrorInjector] Dropped wire xmit of packet "
                      f"#{n} (sender will retransmit)")
                return True  # swallow this transmission; pretend it went out
            self.stats.packets_corrupted += 1
            print(f"[ErrorInjector] Corrupted wire xmit of packet "
                  f"#{n} (sender will retransmit)")
            return orig_send_raw(pkt_type, self._corrupt_data(payload))

        self.pkt.send_raw = one_shot_send_raw
        try:
            return self.pkt.send_packet(data, timeout)
        finally:
            self.pkt.send_raw = orig_send_raw

    def receive_packet(self, timeout: Optional[float] = None) -> bytes:
        """
        Receive data packet with acknowledgment (with delay injection).

        Args:
            timeout: Timeout in seconds

        Returns:
            Received payload data

        Raises:
            PacketError: On timeout or protocol error
        """
        self.stats.packets_received += 1

        if self._should_delay():
            self.stats.packets_delayed += 1
            delay = self.config.delay_seconds
            print(f"[ErrorInjector] Delaying receive by {delay}s")
            time.sleep(delay)

        return self.pkt.receive_packet(timeout)

    def send_ack(self) -> bool:
        """Proxy to underlying send_ack."""
        return self.pkt.send_ack()

    def send_nak(self) -> bool:
        """Proxy to underlying send_nak."""
        return self.pkt.send_nak()

    def flush(self):
        """Proxy to underlying flush."""
        self.pkt.flush()

    def get_stats(self) -> ErrorStats:
        """Get error injection statistics."""
        return self.stats

    def print_stats(self):
        """Print error injection statistics."""
        print("\n[ErrorInjector Statistics]")
        print(f"  Packets sent: {self.stats.packets_sent}")
        print(f"  Packets dropped: {self.stats.packets_dropped}")
        print(f"  Packets corrupted: {self.stats.packets_corrupted}")
        print(f"  Packets delayed: {self.stats.packets_delayed}")
        print(f"  Packets received: {self.stats.packets_received}")


# Preset configurations for common test scenarios
class ErrorPresets:
    """Preset error configurations for testing."""

    @staticmethod
    def no_errors() -> ErrorConfig:
        """No error injection - baseline testing."""
        return ErrorConfig()

    @staticmethod
    def single_drop() -> ErrorConfig:
        """Drop one packet (packet #2, the first DATA packet)."""
        return ErrorConfig(drop_packets=[2])

    @staticmethod
    def single_corrupt() -> ErrorConfig:
        """Corrupt one packet (packet #2, the first DATA packet)."""
        return ErrorConfig(corrupt_packets=[2])

    @staticmethod
    def single_delay() -> ErrorConfig:
        """Delay one receive (tests timeout/retry)."""
        return ErrorConfig(delay_packets=[1], delay_seconds=3.0)

    @staticmethod
    def light_errors() -> ErrorConfig:
        """Light error injection - 5% drop, 2% corrupt."""
        return ErrorConfig(
            drop_probability=0.05,
            corrupt_probability=0.02,
            max_drops=2,
            max_corrupts=1
        )

    @staticmethod
    def heavy_errors() -> ErrorConfig:
        """Heavy error injection - 15% drop, 10% corrupt."""
        return ErrorConfig(
            drop_probability=0.15,
            corrupt_probability=0.10,
            max_drops=5,
            max_corrupts=3
        )

    @staticmethod
    def multiple_drops() -> ErrorConfig:
        """Drop packets 2, 4, and 6."""
        return ErrorConfig(drop_packets=[2, 4, 6])
