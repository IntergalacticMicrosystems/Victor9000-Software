#!/usr/bin/env python3
"""Complete file transfer test with MCP control and debugging."""

import socket
import json
import time
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
sys.path.insert(0, str(Path(__file__).parent.parent.parent / 'serial' / 'python'))

from v9kserial import MamePtyConnection, PacketProtocol
from v9kserial.constants import PKT_TYPE_DATA, PKT_TYPE_ACK
from v9kfiletrx.protocol import (
    StartPacket, DataPacket, EndPacket, ReadyPacket,
    Command, Direction, Status, Compression, parse_packet, CHUNK_SIZE
)
from v9kfiletrx.crc32 import crc32


class MCPClient:
    def __init__(self, host='localhost', port=12345):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(10)
        self.sock.connect((host, port))

    def send(self, method, params=None):
        cmd = {"method": method}
        if params:
            cmd["params"] = params
        self.sock.send((json.dumps(cmd) + "\n").encode())
        time.sleep(0.3)

        self.sock.settimeout(3)
        data = b''
        try:
            while True:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
                data += chunk
                if b'\n' in data:
                    break
        except socket.timeout:
            pass

        if data:
            try:
                return json.loads(data.decode().strip())
            except:
                return {"raw": data.decode()}
        return None

    def screen(self):
        resp = self.send("screen_text")
        if resp and 'result' in resp:
            return resp['result'].get('text', '')
        return ''

    def type_text(self, text):
        self.send("type_text", {"text": text})

    def press_key(self, key):
        self.send("press_key", {"key": key})


def hex_dump(data, max_len=48):
    if len(data) > max_len:
        return ' '.join(f'{b:02X}' for b in data[:max_len]) + f'... ({len(data)} bytes)'
    return ' '.join(f'{b:02X}' for b in data)


def main():
    print("=" * 60)
    print("COMPLETE FILE TRANSFER TEST WITH DEBUG OUTPUT")
    print("=" * 60)

    # Step 1: Connect to MCP and set up Victor
    print("\n[1] Connecting to MCP plugin...")
    try:
        mcp = MCPClient()
        print("    Connected to MCP at localhost:12345")
    except Exception as e:
        print(f"    ERROR: {e}")
        return 1

    # Step 2: Navigate and launch FTXTEST
    print("\n[2] Setting up Victor (launching FTXTEST)...")

    # Type commands
    mcp.type_text("c:")
    mcp.press_key("ENTER")
    time.sleep(0.5)

    mcp.type_text("cd \\bin")
    mcp.press_key("ENTER")
    time.sleep(0.5)

    mcp.type_text("ftxtest")
    mcp.press_key("ENTER")
    time.sleep(3)

    print("    FTXTEST should be running...")

    # Select option 1 (Receive)
    print("    Selecting option 1 (Receive file)...")
    mcp.type_text("1")
    time.sleep(1)

    # Press Enter to accept default filename
    print("    Pressing Enter for default filename...")
    mcp.press_key("ENTER")
    time.sleep(1)

    print("    Victor should be waiting for file transfer...")

    # Step 3: Connect to PTY
    print("\n[3] Connecting to MAME PTY...")
    conn = MamePtyConnection(baudrate=9600)
    conn.connect()
    print(f"    Connected to PTY: {conn.port}")

    pkt = PacketProtocol(conn, timeout=15.0, retries=3)

    # Step 4: Create test data
    print("\n[4] Preparing test data...")
    test_data = b"Hello from Python file transfer test!\nLine 2\nLine 3 - End.\n"
    file_crc = crc32(test_data)
    print(f"    Test data: {len(test_data)} bytes")
    print(f"    CRC-32: 0x{file_crc:08X}")

    # Step 5: Send START packet
    print("\n[5] Sending START packet...")
    start = StartPacket(
        direction=Direction.PC_TO_VICTOR,
        compression=Compression.NONE,
        flags=0x01,  # overwrite
        file_size=len(test_data),
        compressed_size=0,
        file_date=0,
        file_time=0,
        file_attr=0x20,
        filename="TEST.TXT",
        file_crc=file_crc
    )
    start_bytes = start.pack()
    print(f"    START packet: {len(start_bytes)} bytes")
    print(f"    {hex_dump(start_bytes)}")

    try:
        pkt.send_packet(start_bytes)
        print("    START sent and ACKed!")
    except Exception as e:
        print(f"    ERROR sending START: {e}")
        conn.disconnect()
        return 1

    # Step 6: Wait for READY
    print("\n[6] Waiting for READY response...")
    try:
        resp_bytes = pkt.receive_packet(timeout=10.0)
        print(f"    Received: {hex_dump(resp_bytes)}")
        resp = parse_packet(resp_bytes)
        if isinstance(resp, ReadyPacket):
            print(f"    READY received, status={resp.status}")
            if resp.status != 0:
                print(f"    ERROR: Victor rejected transfer!")
                conn.disconnect()
                return 1
        else:
            print(f"    ERROR: Unexpected response type: {type(resp)}")
            conn.disconnect()
            return 1
    except Exception as e:
        print(f"    ERROR waiting for READY: {e}")
        conn.disconnect()
        return 1

    # Step 7: Send DATA chunk
    print("\n[7] Sending DATA chunk 0...")
    data_pkt = DataPacket(chunk_num=0, data=test_data)
    data_bytes = data_pkt.pack()
    print(f"    DATA packet: {len(data_bytes)} bytes")
    print(f"    {hex_dump(data_bytes)}")

    try:
        pkt.send_packet(data_bytes)
        print("    DATA sent and ACKed!")
    except Exception as e:
        print(f"    ERROR sending DATA: {e}")
        conn.disconnect()
        return 1

    # Step 8: Small delay to let Victor process
    print("\n[8] Waiting 1 second for Victor to process data...")
    time.sleep(1.0)

    # Step 9: Send END packet
    print("\n[9] Sending END packet...")
    end = EndPacket(
        total_chunks=1,
        bytes_sent=len(test_data),
        file_crc=file_crc,
        status=Status.OK
    )
    end_bytes = end.pack()
    print(f"    END packet: {len(end_bytes)} bytes")
    print(f"    {hex_dump(end_bytes)}")

    try:
        pkt.send_packet(end_bytes)
        print("    END sent and ACKed!")
    except Exception as e:
        print(f"    ERROR sending END: {e}")
        print(f"    Last error: {pkt.last_error}")

        # Check Victor screen
        print("\n[!] Checking Victor screen...")
        screen = mcp.screen()
        if screen:
            print(f"    Victor screen:\n{screen[:500]}")
        else:
            print("    (could not get screen)")

        conn.disconnect()
        return 1

    # Step 10: Success!
    print("\n" + "=" * 60)
    print("FILE TRANSFER COMPLETED SUCCESSFULLY!")
    print("=" * 60)

    # Check Victor screen for confirmation
    time.sleep(0.5)
    screen = mcp.screen()
    if screen:
        print(f"\nVictor screen:\n{screen[:500]}")

    conn.disconnect()
    return 0


if __name__ == '__main__':
    sys.exit(main())
