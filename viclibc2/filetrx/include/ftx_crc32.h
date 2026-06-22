/**
 * @file ftx_crc32.h
 * @brief CRC-32 Implementation for File Transfer
 *
 * Standard CRC-32 (IEEE 802.3) for file integrity verification.
 * Uses table-driven implementation for speed on 8086.
 *
 * Polynomial: 0xEDB88320 (reflected)
 * Initial value: 0xFFFFFFFF
 * XOR output: 0xFFFFFFFF
 */

#ifndef FTX_CRC32_H
#define FTX_CRC32_H

#include <stdint.h>

/**
 * Calculate CRC-32 of a data buffer.
 * @param data Pointer to data
 * @param len Length of data in bytes
 * @return 32-bit CRC value
 */
uint32_t ftx_crc32(const uint8_t *data, uint32_t len);

/**
 * Update running CRC-32 with additional data.
 * Use this for computing CRC incrementally (e.g., chunk by chunk).
 *
 * Example:
 *   uint32_t crc = FTX_CRC32_INIT;
 *   crc = ftx_crc32_update(crc, chunk1, len1);
 *   crc = ftx_crc32_update(crc, chunk2, len2);
 *   crc ^= FTX_CRC32_INIT;  // Finalize
 *
 * @param crc Current CRC value
 * @param data Pointer to data
 * @param len Length of data in bytes
 * @return Updated CRC value
 */
uint32_t ftx_crc32_update(uint32_t crc, const uint8_t *data, uint32_t len);

/**
 * Update CRC-32 with a single byte.
 * @param crc Current CRC value
 * @param byte Byte to add
 * @return Updated CRC value
 */
uint32_t ftx_crc32_byte(uint32_t crc, uint8_t byte);

/* Initial/final XOR value */
#define FTX_CRC32_INIT  0xFFFFFFFFUL

#endif /* FTX_CRC32_H */
