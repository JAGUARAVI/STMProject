/**
 * @file  bsp_store.h
 * @brief Non-volatile calibration store (replaces ESP32 NVS).
 *
 * One reserved 2 KB flash page, append-only records of
 *   { magic, version, len, crc32, payload }
 * written as 64-bit double-words (the flash program granularity). The newest
 * record with a valid CRC wins at boot. When the page fills, it is erased once
 * and rewriting starts from the beginning, which gives ~100+ updates per erase
 * cycle and atomicity from the CRC.
 */

#ifndef ESWCAP_BSP_STORE_H
#define ESWCAP_BSP_STORE_H

#include <stdint.h>
#include <stdbool.h>

/** Validate the backing page and locate the newest record. */
bool store_init(void);

/** Read the newest valid payload.
 *  @param out      destination
 *  @param len      in: capacity, out: actual length
 *  @param version  receives the record version
 *  @return false if no valid record exists. */
bool store_read(void *out, uint16_t *len, uint16_t *version);

/** Append a record. Erases the page first if full.
 *  @return false on flash failure. */
bool store_write(const void *data, uint16_t len, uint16_t version);

/** Erase the whole store. */
bool store_erase(void);

/** Number of records written since the last erase (wear diagnostics). */
uint32_t store_write_count(void);

/** CRC-32 (IEEE 802.3, reflected) over a buffer. Also used by the host tests. */
uint32_t store_crc32(const void *data, uint32_t len);

#endif /* ESWCAP_BSP_STORE_H */
