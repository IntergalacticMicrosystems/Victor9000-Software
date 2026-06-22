#!/usr/bin/env python3
"""
Automated File Transfer Test Suite

Fully automated testing with no Victor-side user interaction required.
Victor runs FTXTEST.EXE which auto-starts in test server mode.

Usage:
    python test_suite.py                    # MAME PTY mode (default)
    python test_suite.py --iterations 10   # 10 iterations each test
    python test_suite.py --baud 19200      # Test at 19200 baud
    python test_suite.py --errors          # Include error injection tests
    python test_suite.py --port COM15      # Windows serial port (auto-detects real)
    python test_suite.py --port /dev/ttyUSB0 --connection real  # Linux serial
"""

import sys
import time
import secrets
import argparse
from pathlib import Path
from dataclasses import dataclass, field
from datetime import datetime
from typing import Optional, List
from enum import Enum, auto

# Add library paths
sys.path.insert(0, str(Path(__file__).parent))
sys.path.insert(0, str(Path(__file__).parent.parent.parent / 'serial' / 'python'))

from v9kserial import MamePtyConnection, SerialConnection, PacketProtocol
from v9kfiletrx import (
    FileTransfer, Compression,
    TestCmdPacket, TestResultPacket, TestResult as TestResultCode,
    create_ping, is_pong, baud_to_index,
    COMPRESS_NONE, COMPRESS_RLE,
    ErrorInjector, ErrorConfig, ErrorPresets
)
from v9kfiletrx.crc32 import crc32


class ConnectionType(Enum):
    """Connection type for testing."""
    MAME_PTY = auto()
    REAL_SERIAL = auto()


@dataclass
class TestConfig:
    """Global test configuration."""
    iterations: int = 3
    baud_rate: int = 9600
    connection_type: ConnectionType = ConnectionType.MAME_PTY
    serial_port: str = "/dev/ttyUSB0"
    include_error_tests: bool = False
    compression_modes: List[int] = field(default_factory=lambda: [COMPRESS_NONE, COMPRESS_RLE])


@dataclass
class SingleTestResult:
    """Result of a single test."""
    test_name: str
    iteration: int
    passed: bool
    duration_seconds: float
    bytes_transferred: int = 0
    speed_bps: float = 0.0
    victor_retries: int = 0
    victor_errors: int = 0
    pc_retries: int = 0
    error_message: str = ""
    error_preset: str = ""


class TestController:
    """
    Controls Victor test server and runs automated tests.
    """

    def __init__(self, config: TestConfig):
        self.config = config
        self.conn = None
        self.pkt = None
        self.ftx = None
        self.results: List[SingleTestResult] = []
        self.test_dir = Path(__file__).parent / "test_files"
        self.results_dir = Path(__file__).parent / "results"
        self.start_time: Optional[datetime] = None

    def setup(self):
        """Set up test environment."""
        self.test_dir.mkdir(exist_ok=True)
        self.results_dir.mkdir(exist_ok=True)
        self.generate_test_files()
        self.start_time = datetime.now()

    def generate_test_files(self):
        """Generate test data files of various sizes."""
        print("Generating test files...")

        # Small: 500 bytes (single chunk)
        small_data = bytes(range(256)) + bytes(range(244))
        (self.test_dir / "test_small.bin").write_bytes(small_data)
        print(f"  test_small.bin: {len(small_data)} bytes")

        # Medium: 5KB (~5 chunks)
        medium_data = bytes(range(256)) * 20
        (self.test_dir / "test_medium.bin").write_bytes(medium_data)
        print(f"  test_medium.bin: {len(medium_data)} bytes")

        # Large: 20KB (~20 chunks)
        large_data = bytes(range(256)) * 80
        (self.test_dir / "test_large.bin").write_bytes(large_data)
        print(f"  test_large.bin: {len(large_data)} bytes")

        # Extra large: 50KB (~50 chunks)
        xlarge_data = bytes(range(256)) * 200
        (self.test_dir / "test_xlarge.bin").write_bytes(xlarge_data)
        print(f"  test_xlarge.bin: {len(xlarge_data)} bytes")

        # Highly compressible: 5KB of repeated patterns
        compress_data = b'A' * 1024 + b'B' * 1024 + b'\x00' * 1024 + b'\xFF' * 1024 + b'X' * 1024
        (self.test_dir / "test_compress.bin").write_bytes(compress_data)
        print(f"  test_compress.bin: {len(compress_data)} bytes (compressible)")

        print("Test files generated.")

    def connect(self) -> bool:
        """Connect to Victor based on configuration."""
        try:
            print(f"Connecting ({self.config.connection_type.name})...")

            if self.config.connection_type == ConnectionType.MAME_PTY:
                self.conn = MamePtyConnection(baudrate=self.config.baud_rate)
            else:
                self.conn = SerialConnection(
                    port=self.config.serial_port,
                    baudrate=self.config.baud_rate
                )

            self.conn.connect()
            self.pkt = PacketProtocol(self.conn, timeout=15.0, retries=5)
            # Resync sequence bits in case the Victor test server is still
            # running from a previous host session (best-effort: older
            # servers without RESET support just ignore it).
            if self.pkt.reset():
                print("  Sequence bits resynced (RESET ack'd)")
            self.ftx = FileTransfer(self.pkt)

            if self.config.connection_type == ConnectionType.MAME_PTY:
                print(f"  Connected to PTY: {self.conn.port}")
            else:
                print(f"  Connected to: {self.config.serial_port}")

            return True

        except Exception as e:
            print(f"  Connection failed: {e}")
            return False

    def disconnect(self):
        """Disconnect from Victor."""
        if self.conn:
            try:
                self.conn.disconnect()
            except:
                pass
            self.conn = None
            self.pkt = None
            self.ftx = None

    def send_test_cmd(self, cmd: TestCmdPacket, timeout: float = 30.0) -> TestResultPacket:
        """Send test command and wait for result."""
        self.pkt.send_packet(cmd.pack(), timeout=timeout)
        resp_data = self.pkt.receive_packet(timeout=timeout)
        return TestResultPacket.unpack(resp_data)

    def ping_victor(self, timeout: float = 5.0) -> bool:
        """Ping Victor to check if test server is ready."""
        try:
            ping_data = create_ping()
            self.pkt.send_packet(ping_data, timeout=timeout)
            resp = self.pkt.receive_packet(timeout=timeout)
            return is_pong(resp)
        except Exception as e:
            print(f"  Ping failed: {e}")
            return False

    def configure_victor(self, compression: int) -> bool:
        """Configure Victor's compression mode."""
        try:
            cmd = TestCmdPacket.set_compress(compression)
            result = self.send_test_cmd(cmd)
            if not result.success:
                print(f"  Failed to set compression: {result.message}")
                return False
            print(f"  Compression: {result.message}")
            return True
        except Exception as e:
            print(f"  Configuration error: {e}")
            return False

    def run_pc_to_victor_test(
        self,
        test_file: Path,
        compression: int,
        iteration: int,
        error_config: Optional[ErrorConfig] = None
    ) -> SingleTestResult:
        """Run a PC-to-Victor transfer test."""
        test_name = f"PC->Victor:{test_file.name}:comp={compression}"
        result = SingleTestResult(
            test_name=test_name,
            iteration=iteration,
            passed=False,
            duration_seconds=0.0
        )

        if error_config:
            result.error_preset = "errors"

        try:
            # Tell Victor to prepare for receive
            cmd = TestCmdPacket.recv_file(test_file.name.upper())
            prep_result = self.send_test_cmd(cmd)
            if not prep_result.success:
                result.error_message = f"Victor not ready: {prep_result.message}"
                return result

            # Wrap protocol with error injector if configured
            protocol = self.pkt
            if error_config:
                protocol = ErrorInjector(self.pkt, error_config)

            ftx = FileTransfer(protocol)

            # Send file
            start_time = time.time()
            stats = ftx.send_file(
                test_file,
                test_file.name.upper(),
                compression=Compression(compression),
                overwrite=True
            )

            result.duration_seconds = time.time() - start_time
            result.bytes_transferred = stats.bytes_transferred
            result.speed_bps = stats.bytes_per_second
            result.pc_retries = stats.retries

            # Request Victor's transfer result using GET_RESULT command
            # This ensures reliable bidirectional confirmation
            get_result_cmd = TestCmdPacket.get_result()
            self.pkt.send_packet(get_result_cmd.pack())
            victor_response = self.pkt.receive_packet(timeout=5.0)

            if victor_response and len(victor_response) > 0:
                victor_result = TestResultPacket.unpack(victor_response)
                result.victor_retries = victor_result.retries
                result.victor_errors = victor_result.errors
                result.passed = victor_result.success
                if not result.passed:
                    result.error_message = f"Victor: {victor_result.message}"
            else:
                result.passed = False
                result.error_message = "No result from Victor"

        except Exception as e:
            result.error_message = str(e)
            result.duration_seconds = time.time() - start_time if 'start_time' in locals() else 0

        return result

    def run_victor_to_pc_test(
        self,
        filename: str,
        compression: int,
        iteration: int,
        error_config: Optional[ErrorConfig] = None
    ) -> SingleTestResult:
        """Run a Victor-to-PC transfer test."""
        test_name = f"Victor->PC:{filename}:comp={compression}"
        result = SingleTestResult(
            test_name=test_name,
            iteration=iteration,
            passed=False,
            duration_seconds=0.0
        )

        if error_config:
            result.error_preset = "errors"

        try:
            # Wrap protocol with error injector if configured
            protocol = self.pkt
            if error_config:
                protocol = ErrorInjector(self.pkt, error_config)

            ftx = FileTransfer(protocol)

            # Tell Victor to send file
            cmd = TestCmdPacket.send_file(filename)
            self.pkt.send_packet(cmd.pack())

            # Receive file
            output_path = self.test_dir / f"recv_{filename.lower()}"
            start_time = time.time()
            stats = ftx.receive_file(filename, output_path)

            result.duration_seconds = time.time() - start_time
            result.bytes_transferred = stats.bytes_transferred
            result.speed_bps = stats.bytes_per_second
            result.pc_retries = stats.retries
            result.passed = True

            # Get Victor's final result
            try:
                victor_result = self.pkt.receive_packet(timeout=10.0)
                victor_result = TestResultPacket.unpack(victor_result)
                result.victor_retries = victor_result.retries
                result.victor_errors = victor_result.errors
            except:
                pass  # Victor result is optional here

        except Exception as e:
            result.error_message = str(e)
            result.duration_seconds = time.time() - start_time if 'start_time' in locals() else 0

        return result

    def run_all_tests(self) -> List[SingleTestResult]:
        """Run the complete test suite."""
        results = []

        # Test files
        test_files = [
            self.test_dir / "test_small.bin",
            self.test_dir / "test_medium.bin",
            self.test_dir / "test_large.bin",
            self.test_dir / "test_xlarge.bin",
            self.test_dir / "test_compress.bin",
        ]

        # Error presets
        error_presets = [
            ("baseline", None),
        ]
        if self.config.include_error_tests:
            error_presets.extend([
                ("single_drop", ErrorPresets.single_drop()),
                ("single_corrupt", ErrorPresets.single_corrupt()),
                ("multiple_drops", ErrorPresets.multiple_drops()),
                ("light_errors", ErrorPresets.light_errors()),
            ])

        total_tests = (
            len(self.config.compression_modes) *
            len(error_presets) *
            len(test_files) *
            self.config.iterations
        )
        test_num = 0

        for compression in self.config.compression_modes:
            comp_name = "RLE" if compression == COMPRESS_RLE else "None"
            print(f"\n{'='*60}")
            print(f"Compression: {comp_name}")
            print(f"{'='*60}")

            # Configure Victor
            if not self.configure_victor(compression):
                print("  WARNING: Could not configure Victor")

            for preset_name, error_config in error_presets:
                if error_config:
                    print(f"\n--- Error preset: {preset_name} ---")

                for test_file in test_files:
                    for iteration in range(1, self.config.iterations + 1):
                        test_num += 1
                        progress = f"[{test_num}/{total_tests}]"

                        # PC -> Victor test
                        print(f"{progress} {test_file.name} (iter {iteration})...", end=" ", flush=True)

                        result = self.run_pc_to_victor_test(
                            test_file, compression, iteration, error_config
                        )
                        results.append(result)

                        if result.passed:
                            print(f"PASS ({result.speed_bps:.0f} B/s, retries={result.pc_retries})")
                        else:
                            print(f"FAIL: {result.error_message}")

                        # Brief delay between tests
                        time.sleep(0.3)

        self.results = results
        return results

    def generate_report(self) -> str:
        """Generate comprehensive test report."""
        end_time = datetime.now()
        duration = (end_time - self.start_time).total_seconds() if self.start_time else 0

        lines = [
            "=" * 70,
            "File Transfer Test Report",
            f"Generated: {end_time.strftime('%Y-%m-%d %H:%M:%S')}",
            "=" * 70,
            "",
            "CONFIGURATION",
            "-" * 40,
            f"  Iterations: {self.config.iterations}",
            f"  Baud Rate: {self.config.baud_rate}",
            f"  Connection: {self.config.connection_type.name}",
            f"  Error Tests: {'Yes' if self.config.include_error_tests else 'No'}",
            f"  Compression Modes: {', '.join('RLE' if c else 'None' for c in self.config.compression_modes)}",
            "",
        ]

        # Summary statistics
        passed = sum(1 for r in self.results if r.passed)
        failed = sum(1 for r in self.results if not r.passed)
        total = len(self.results)

        if self.results:
            speeds = [r.speed_bps for r in self.results if r.passed and r.speed_bps > 0]
            avg_speed = sum(speeds) / len(speeds) if speeds else 0
            total_bytes = sum(r.bytes_transferred for r in self.results if r.passed)
            total_retries = sum(r.pc_retries + r.victor_retries for r in self.results)

            lines.extend([
                "=" * 70,
                "SUMMARY",
                "=" * 70,
                f"  Total Tests: {total}",
                f"  Passed: {passed}",
                f"  Failed: {failed}",
                f"  Pass Rate: {100*passed/total:.1f}%" if total > 0 else "  Pass Rate: N/A",
                f"  Average Speed: {avg_speed:.0f} B/s",
                f"  Total Data: {total_bytes:,} bytes",
                f"  Total Retries: {total_retries}",
                f"  Test Duration: {duration:.1f}s",
                "",
            ])

        # Group by compression mode
        for compression in self.config.compression_modes:
            comp_name = "RLE" if compression == COMPRESS_RLE else "None"
            comp_results = [r for r in self.results if f":comp={compression}" in r.test_name]
            comp_passed = sum(1 for r in comp_results if r.passed)
            comp_total = len(comp_results)

            lines.append(f"  Compression {comp_name}: {comp_passed}/{comp_total} passed")

        lines.append("")

        # Failed tests
        failed_results = [r for r in self.results if not r.passed]
        if failed_results:
            lines.extend([
                "=" * 70,
                "FAILED TESTS",
                "=" * 70,
            ])
            for result in failed_results:
                lines.append(f"  [FAIL] {result.test_name} (iter {result.iteration})")
                lines.append(f"         Error: {result.error_message}")
                if result.error_preset:
                    lines.append(f"         Preset: {result.error_preset}")
            lines.append("")

        # Detailed results (condensed)
        lines.extend([
            "=" * 70,
            "DETAILED RESULTS",
            "=" * 70,
        ])

        for result in self.results:
            status = "PASS" if result.passed else "FAIL"
            lines.append(
                f"[{status}] {result.test_name} iter={result.iteration} "
                f"bytes={result.bytes_transferred} speed={result.speed_bps:.0f}B/s "
                f"retries={result.pc_retries}"
            )

        lines.extend([
            "",
            "=" * 70,
            f"FINAL STATUS: {'ALL TESTS PASSED' if failed == 0 else f'{failed} TESTS FAILED'}",
            "=" * 70,
        ])

        return "\n".join(lines)

    def quit_victor_server(self):
        """Tell Victor to exit test server mode."""
        try:
            cmd = TestCmdPacket.quit()
            self.pkt.send_packet(cmd.pack(), timeout=5.0)
            # Don't wait for response - Victor may exit immediately
        except:
            pass


def main():
    parser = argparse.ArgumentParser(
        description="Automated File Transfer Test Suite",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python test_suite.py                              # MAME PTY with defaults
  python test_suite.py --iterations 5 --baud 19200 # 5 iterations at 19200 baud
  python test_suite.py --errors                    # Include error injection tests
  python test_suite.py --port COM15                # Windows serial port (auto-detects real)
  python test_suite.py --port /dev/ttyUSB0 --connection real  # Linux serial port
        """
    )
    parser.add_argument("--iterations", "-n", type=int, default=3,
                       help="Number of iterations per test (default: 3)")
    parser.add_argument("--baud", "-b", type=int, default=9600,
                       choices=[9600, 19200, 38400],
                       help="Baud rate (default: 9600). NOTE: real hardware is "
                            "reliable only at 9600; the Victor's interrupt-driven "
                            "SIO receive overruns at 19200+ (see baudbench.py).")
    parser.add_argument("--connection", "-c", choices=["mame", "real"],
                       default="mame",
                       help="Connection type (default: mame, auto-detects 'real' for COM ports)")
    parser.add_argument("--port", "-p", type=str, default=None,
                       help="Serial port (e.g., COM15 on Windows, /dev/ttyUSB0 on Linux)")
    parser.add_argument("--errors", "-e", action="store_true",
                       help="Include error injection tests")
    parser.add_argument("--compression", choices=["none", "rle", "both"],
                       default="both",
                       help="Compression modes to test (default: both)")
    parser.add_argument("--generate-only", action="store_true",
                       help="Only generate test files, don't run tests")
    parser.add_argument("--no-quit", action="store_true",
                       help="Don't send quit command to Victor at end")

    args = parser.parse_args()

    # Auto-detect connection type if port is specified
    # Windows COM ports or explicit --connection real use REAL_SERIAL
    if args.connection == "real" or (args.port and args.port.upper().startswith("COM")):
        conn_type = ConnectionType.REAL_SERIAL
        serial_port = args.port if args.port else "/dev/ttyUSB0"
    else:
        conn_type = ConnectionType.MAME_PTY
        serial_port = args.port  # Not used for MAME_PTY

    # Build config
    config = TestConfig(
        iterations=args.iterations,
        baud_rate=args.baud,
        connection_type=conn_type,
        serial_port=serial_port,
        include_error_tests=args.errors,
    )

    if args.compression == "none":
        config.compression_modes = [COMPRESS_NONE]
    elif args.compression == "rle":
        config.compression_modes = [COMPRESS_RLE]
    else:
        config.compression_modes = [COMPRESS_NONE, COMPRESS_RLE]

    # Create controller
    controller = TestController(config)
    controller.setup()

    if args.generate_only:
        print("Test files generated. Exiting.")
        return 0

    try:
        # Connect
        print("\nConnecting to Victor...")
        if not controller.connect():
            print("\nERROR: Failed to connect to Victor.")
            print("Make sure MAME is running with mcpplugin and FTXTEST.EXE is running.")
            return 1

        # Ping Victor test server
        print("Pinging Victor test server...")
        if not controller.ping_victor():
            print("\nERROR: Victor test server not responding.")
            print("Please ensure FTXTEST.EXE is running on Victor.")
            print("The program should display 'Waiting for commands from PC...'")
            return 1

        print("Victor test server ready!")

        # Run all tests
        print("\nStarting test suite...")
        results = controller.run_all_tests()

        # Generate and save report
        report = controller.generate_report()
        print("\n" + report)

        report_dir = controller.results_dir
        report_file = report_dir / f"report_{datetime.now().strftime('%Y%m%d_%H%M%S')}.txt"
        report_file.write_text(report)
        print(f"\nReport saved to: {report_file}")

        # Return exit code
        failed = sum(1 for r in results if not r.passed)
        return 0 if failed == 0 else 1

    finally:
        if not args.no_quit:
            print("\nSending quit command to Victor...")
            controller.quit_victor_server()
        controller.disconnect()


if __name__ == "__main__":
    sys.exit(main())
