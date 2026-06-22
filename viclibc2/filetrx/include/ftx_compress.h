/**
 * @file ftx_compress.h
 * @brief RLE Compression for File Transfer
 *
 * Simple RLE (Run-Length Encoding) compression designed for
 * fast decompression on 5MHz 8086.
 *
 * Encoding format:
 *   Regular byte:     [byte]              (if byte != 0x90)
 *   Escape sequence:  [0x90][count][byte] (repeat byte count+3 times)
 *   Literal 0x90:     [0x90][0x00]        (outputs single 0x90)
 *
 * Minimum run length: 4 (saves bytes only for runs >= 4)
 * Maximum run length: 258 (count byte 255 + 3)
 */

#ifndef FTX_COMPRESS_H
#define FTX_COMPRESS_H

#include <stdint.h>

/* RLE escape byte */
#define RLE_ESCAPE      0x90

/* Minimum run length to encode (shorter runs not worth encoding) */
#define RLE_MIN_RUN     4

/* Maximum run length (255 + 3 = 258) */
#define RLE_MAX_RUN     258

/*===========================================================================
 * Compression Functions
 *===========================================================================*/

/**
 * Compress data using RLE.
 *
 * @param src       Source data
 * @param src_len   Source data length
 * @param dst       Destination buffer
 * @param dst_max   Maximum destination buffer size
 * @return Compressed size, or 0 on error (buffer too small)
 *
 * Note: If compressed size >= src_len, data is not compressible.
 * Caller should check return value and use uncompressed if larger.
 */
uint32_t rle_compress(const uint8_t *src, uint32_t src_len,
                      uint8_t *dst, uint32_t dst_max);

/**
 * Decompress RLE-compressed data.
 *
 * @param src       Compressed data
 * @param src_len   Compressed data length
 * @param dst       Destination buffer
 * @param dst_max   Maximum destination buffer size
 * @return Decompressed size, or 0 on error
 *
 * Designed for fast execution on 8086.
 */
uint32_t rle_decompress(const uint8_t *src, uint32_t src_len,
                        uint8_t *dst, uint32_t dst_max);

/**
 * Estimate maximum compressed size.
 * Worst case: every byte is the escape byte (2x expansion).
 *
 * @param src_len   Original data length
 * @return Maximum possible compressed size
 */
uint32_t rle_max_compressed_size(uint32_t src_len);

#endif /* FTX_COMPRESS_H */
