"""
RLE Compression Implementation.

Simple RLE (Run-Length Encoding) compression compatible with
the C implementation in ftx_compress.c.

Encoding format:
    Regular byte:     [byte]              (if byte != 0x90)
    Escape sequence:  [0x90][count][byte] (repeat byte count+3 times)
    Literal 0x90:     [0x90][0x00]        (outputs single 0x90)

Minimum run length: 4 (saves bytes only for runs >= 4)
Maximum run length: 258 (count byte 255 + 3)
"""

# RLE escape byte
RLE_ESCAPE = 0x90

# Minimum run length to encode
RLE_MIN_RUN = 4

# Maximum run length (255 + 3)
RLE_MAX_RUN = 258


def rle_compress(data: bytes) -> bytes:
    """
    Compress data using RLE.

    Args:
        data: Data to compress

    Returns:
        Compressed data, or original if not compressible
    """
    if not data:
        return b''

    result = bytearray()
    i = 0
    n = len(data)

    while i < n:
        byte = data[i]
        run_len = 1

        # Count run length
        while i + run_len < n and data[i + run_len] == byte and run_len < RLE_MAX_RUN:
            run_len += 1

        if run_len >= RLE_MIN_RUN:
            # Encode as run: [ESCAPE][count-3][byte]
            result.append(RLE_ESCAPE)
            result.append(run_len - 3)
            result.append(byte)
            i += run_len
        else:
            # Output literal bytes
            while run_len > 0:
                if byte == RLE_ESCAPE:
                    # Escape the escape byte
                    result.append(RLE_ESCAPE)
                    result.append(0x00)
                else:
                    result.append(byte)
                i += 1
                run_len -= 1

                # Check for start of new run
                if run_len == 0 and i < n:
                    byte = data[i]
                    run_len = 1
                    while i + run_len < n and data[i + run_len] == byte and run_len < RLE_MAX_RUN:
                        run_len += 1
                    if run_len >= RLE_MIN_RUN:
                        break

    return bytes(result)


def rle_decompress(data: bytes) -> bytes:
    """
    Decompress RLE-compressed data.

    Args:
        data: Compressed data

    Returns:
        Decompressed data

    Raises:
        ValueError: If data is corrupted
    """
    if not data:
        return b''

    result = bytearray()
    i = 0
    n = len(data)

    while i < n:
        byte = data[i]
        i += 1

        if byte == RLE_ESCAPE:
            # Escape sequence
            if i >= n:
                raise ValueError("Truncated RLE data")

            count = data[i]
            i += 1

            if count == 0x00:
                # Literal escape byte
                result.append(RLE_ESCAPE)
            else:
                # Run of bytes
                if i >= n:
                    raise ValueError("Truncated RLE data")
                run_byte = data[i]
                i += 1
                run_len = count + 3
                result.extend([run_byte] * run_len)
        else:
            # Literal byte
            result.append(byte)

    return bytes(result)


def rle_max_compressed_size(src_len: int) -> int:
    """
    Estimate maximum compressed size.

    Worst case: every byte is the escape byte (2x expansion).

    Args:
        src_len: Original data length

    Returns:
        Maximum possible compressed size
    """
    return src_len * 2


def compress_if_beneficial(data: bytes) -> tuple[bytes, bool]:
    """
    Compress data if it reduces size.

    Args:
        data: Data to potentially compress

    Returns:
        Tuple of (data, is_compressed)
        - If compression helped: (compressed_data, True)
        - If not: (original_data, False)
    """
    if not data:
        return b'', False

    compressed = rle_compress(data)

    if len(compressed) < len(data):
        return compressed, True
    else:
        return data, False
