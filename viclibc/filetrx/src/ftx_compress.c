/**
 * @file ftx_compress.c
 * @brief RLE Compression Implementation
 *
 * Simple RLE compression optimized for fast decompression on 8086.
 */

#include "ftx_compress.h"

uint32_t rle_compress(const uint8_t *src, uint32_t src_len,
                      uint8_t *dst, uint32_t dst_max)
{
    uint32_t src_pos;
    uint32_t dst_pos;
    uint8_t byte;
    uint32_t run_len;

    src_pos = 0;
    dst_pos = 0;

    while (src_pos < src_len) {
        byte = src[src_pos];
        run_len = 1;

        /* Count run length */
        while (src_pos + run_len < src_len &&
               src[src_pos + run_len] == byte &&
               run_len < RLE_MAX_RUN) {
            run_len++;
        }

        if (run_len >= RLE_MIN_RUN) {
            /* Encode as run: [ESCAPE][count-3][byte] */
            if (dst_pos + 3 > dst_max) {
                return 0;  /* Buffer overflow */
            }
            dst[dst_pos++] = RLE_ESCAPE;
            dst[dst_pos++] = (uint8_t)(run_len - 3);
            dst[dst_pos++] = byte;
            src_pos += run_len;
        } else {
            /* Output literal bytes */
            while (run_len > 0) {
                if (byte == RLE_ESCAPE) {
                    /* Escape the escape byte: [ESCAPE][0x00] */
                    if (dst_pos + 2 > dst_max) {
                        return 0;  /* Buffer overflow */
                    }
                    dst[dst_pos++] = RLE_ESCAPE;
                    dst[dst_pos++] = 0x00;
                } else {
                    /* Normal byte */
                    if (dst_pos + 1 > dst_max) {
                        return 0;  /* Buffer overflow */
                    }
                    dst[dst_pos++] = byte;
                }
                src_pos++;
                run_len--;

                /* Get next byte if continuing literals */
                if (run_len == 0 && src_pos < src_len) {
                    byte = src[src_pos];
                    /* Check if next is start of a run */
                    run_len = 1;
                    while (src_pos + run_len < src_len &&
                           src[src_pos + run_len] == byte &&
                           run_len < RLE_MAX_RUN) {
                        run_len++;
                    }
                    if (run_len >= RLE_MIN_RUN) {
                        /* Start of a new run, break to handle it */
                        break;
                    }
                }
            }
        }
    }

    return dst_pos;
}

uint32_t rle_decompress(const uint8_t *src, uint32_t src_len,
                        uint8_t *dst, uint32_t dst_max)
{
    uint32_t src_pos;
    uint32_t dst_pos;
    uint8_t byte;
    uint8_t count;
    uint16_t run_len;

    src_pos = 0;
    dst_pos = 0;

    while (src_pos < src_len) {
        byte = src[src_pos++];

        if (byte == RLE_ESCAPE) {
            /* Escape sequence */
            if (src_pos >= src_len) {
                return 0;  /* Truncated data */
            }

            count = src[src_pos++];

            if (count == 0x00) {
                /* Literal escape byte */
                if (dst_pos >= dst_max) {
                    return 0;  /* Buffer overflow */
                }
                dst[dst_pos++] = RLE_ESCAPE;
            } else {
                /* Run of bytes */
                if (src_pos >= src_len) {
                    return 0;  /* Truncated data */
                }
                byte = src[src_pos++];
                run_len = (uint16_t)count + 3;

                if (dst_pos + run_len > dst_max) {
                    return 0;  /* Buffer overflow */
                }

                /* Fast fill loop - optimized for 8086 */
                while (run_len--) {
                    dst[dst_pos++] = byte;
                }
            }
        } else {
            /* Literal byte */
            if (dst_pos >= dst_max) {
                return 0;  /* Buffer overflow */
            }
            dst[dst_pos++] = byte;
        }
    }

    return dst_pos;
}

uint32_t rle_max_compressed_size(uint32_t src_len)
{
    /* Worst case: every byte is the escape byte (2x expansion) */
    return src_len * 2;
}
