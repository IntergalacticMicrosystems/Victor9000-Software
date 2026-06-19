"""
MAME PTY Connection.

Provides connection via MAME-created PTY (pseudo-terminal) for
testing with the Victor 9000 emulator.

MAME creates a PTY when started with serial port emulation:
    ./mame victor9k -rs232a pty

This module auto-detects the PTY device created by MAME.
"""

import glob
import os
import subprocess
import sys
from typing import Optional, Set

from .serial_port import SerialConnection
from .connection import ConnectionError


class MamePtyConnection(SerialConnection):
    """
    Connection via MAME-created PTY.

    Automatically detects the PTY device created by MAME for serial
    port emulation. Inherits all functionality from SerialConnection.

    Example:
        # Auto-detect MAME PTY
        conn = MamePtyConnection(baudrate=9600)
        conn.connect()

        # Or specify PTY path manually
        conn = MamePtyConnection(baudrate=9600, pty_path='/dev/pts/5')
        conn.connect()

    Requires MAME to be running with:
        ./mame victor9k -rs232a pty
    """

    def __init__(self, baudrate: int = 9600, timeout: float = 1.0,
                 pty_path: Optional[str] = None):
        """
        Initialize MAME PTY connection.

        Args:
            baudrate: Baud rate (must match Victor 9000 side)
            timeout: Read timeout in seconds
            pty_path: Explicit PTY path, or None to auto-detect
        """
        super().__init__(port="", baudrate=baudrate, timeout=timeout)
        self._pty_path = pty_path

    def connect(self) -> bool:
        """
        Connect to MAME PTY.

        Auto-detects PTY if not specified explicitly.
        """
        if self._pty_path:
            self.port = self._pty_path
        else:
            self.port = self._find_mame_pty()

        return super().connect()

    @staticmethod
    def _get_controlling_ttys() -> Set[str]:
        """
        Get PTY devices that are controlling terminals of processes.

        These PTYs should be excluded from MAME PTY detection as they
        are used by shells and other interactive programs.
        """
        controlling = set()

        # Add our own terminal
        try:
            if sys.stdin.isatty():
                controlling.add(os.ttyname(sys.stdin.fileno()))
        except OSError:
            pass

        # Read /proc/*/stat to find controlling terminals
        try:
            for proc_dir in glob.glob('/proc/[0-9]*'):
                try:
                    with open(os.path.join(proc_dir, 'stat'), 'r') as f:
                        stat = f.read()
                        # Field 7 is tty_nr (controlling terminal device number)
                        # Format: pid (comm) state ppid pgrp session tty_nr ...
                        close_paren = stat.rfind(')')
                        if close_paren > 0:
                            fields = stat[close_paren + 2:].split()
                            if len(fields) >= 5:
                                tty_nr = int(fields[4])
                                if tty_nr != 0:
                                    # Major 136-143 are PTY slaves
                                    major = (tty_nr >> 8) & 0xff
                                    minor = tty_nr & 0xff
                                    if 136 <= major <= 143:
                                        pts_num = (major - 136) * 256 + minor
                                        controlling.add(f'/dev/pts/{pts_num}')
                except (OSError, PermissionError, ValueError, IndexError):
                    continue
        except (OSError, PermissionError):
            pass

        return controlling

    @classmethod
    def _find_mame_pty(cls) -> str:
        """
        Auto-detect MAME's PTY slave device.

        MAME opens a PTY master (/dev/ptmx) which creates a slave
        (/dev/pts/X). We find the slave that isn't a controlling
        terminal of any process.

        Returns:
            Path to detected PTY device

        Raises:
            ConnectionError: If MAME not running or PTY not found
        """
        # Check if MAME is running
        try:
            result = subprocess.run(
                ['pgrep', '-x', 'mame'],
                capture_output=True,
                text=True,
                timeout=5
            )
            if result.returncode != 0 or not result.stdout.strip():
                raise ConnectionError(
                    "MAME not running. Start with: ./mame victor9k -rs232a pty"
                )
        except FileNotFoundError:
            # pgrep not available, continue anyway
            pass
        except subprocess.TimeoutExpired:
            pass

        # Get controlling terminals to exclude
        controlling = cls._get_controlling_ttys()

        # Find PTY slaves that are NOT controlling terminals
        try:
            pts_devices = glob.glob('/dev/pts/[0-9]*')
            candidates = []

            for pts in pts_devices:
                if pts not in controlling:
                    try:
                        stat = os.stat(pts)
                        candidates.append((pts, stat.st_mtime))
                    except OSError:
                        continue

            if candidates:
                # Sort by modification time, newest first
                candidates.sort(key=lambda x: x[1], reverse=True)
                return candidates[0][0]

        except (OSError, PermissionError):
            pass

        raise ConnectionError(
            "Could not find MAME PTY. "
            "Ensure MAME is running with: ./mame victor9k -rs232a pty"
        )

    @classmethod
    def find_mame_pty(cls) -> Optional[str]:
        """
        Find MAME PTY without raising exception.

        Returns:
            PTY path if found, None otherwise
        """
        try:
            return cls._find_mame_pty()
        except ConnectionError:
            return None

    @classmethod
    def is_mame_running(cls) -> bool:
        """Check if MAME is currently running."""
        try:
            result = subprocess.run(
                ['pgrep', '-x', 'mame'],
                capture_output=True,
                timeout=5
            )
            return result.returncode == 0
        except (FileNotFoundError, subprocess.TimeoutExpired):
            return False
