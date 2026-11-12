/**
 * @file  stubs.c
 * @brief Host-side stubs for the few hardware-backed symbols the pure
 *        measurement modules reference.
 *
 * Only the flash store is stubbed: everything else the maths touches is real
 * code. Keeping the stubs this small is deliberate — if the maths starts
 * depending on the BSP, the test build breaks and that is a signal worth
 * having.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "bsp_store.h"

static uint8_t s_blob[512];
static uint16_t s_blob_len;
static uint16_t s_blob_version;

bool store_init(void)
{
    return true;
}

bool store_read(void *out, uint16_t *len, uint16_t *version)
{
    if (s_blob_len == 0U) {
        return false;
    }
    uint16_t n = (*len < s_blob_len) ? *len : s_blob_len;
    memcpy(out, s_blob, n);
    *len = s_blob_len;
    if (version != NULL) {
        *version = s_blob_version;
    }
    return true;
}

bool store_write(const void *data, uint16_t len, uint16_t version)
{
    if (len > sizeof(s_blob)) {
        return false;
    }
    memcpy(s_blob, data, len);
    s_blob_len = len;
    s_blob_version = version;
    return true;
}

bool store_erase(void)
{
    s_blob_len = 0;
    return true;
}

uint32_t store_write_count(void)
{
    return 0;
}

uint32_t store_crc32(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFUL;
    for (uint32_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 1U) ? ((crc >> 1) ^ 0xEDB88320UL) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFUL;
}
