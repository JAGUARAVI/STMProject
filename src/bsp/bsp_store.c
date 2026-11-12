/**
 * @file  bsp_store.c
 * @brief Non-volatile calibration store in the reserved top flash page.
 *
 * @details
 * One 2 KB region at @ref STORE_FLASH_ADDR, at the top of the 512 KB
 * STM32G474CEU6 flash, holds an append-only sequence of self-describing
 * records:
 *
 * @verbatim
 *   offset  size  field
 *   0       4     magic   = STORE_RECORD_MAGIC ("ESWC")
 *   4       2     version caller-supplied record version
 *   6       2     len     payload length in bytes (0 ... 500)
 *   8       4     crc     CRC-32 (IEEE, reflected) over the payload only
 *   12      len   payload
 *   ...     pad   0xFF up to a multiple of 8 bytes
 * @endverbatim
 *
 * The record header is 12 bytes and the complete record is padded to the
 * 64-bit flash programming granularity, so record n+1 always starts on a
 * double-word boundary.
 *
 * **Why append-only?** Flash bits can only be cleared by programming and can
 * only be set again by a page erase. A calibration update therefore cannot
 * simply overwrite the previous record. Appending a new record costs one
 * programming pass and zero erases; the page is erased only when the next
 * record no longer fits. If a record occupies
 * `align8(12 + len)` bytes, one erase cycle absorbs
 * `floor(2048 / align8(12 + len))` updates in steady state (a torn write can
 * force one extra recovery erase). For a 4-byte payload that is 128
 * updates per erase, for a 64-byte payload 25, and for the maximum 500-byte
 * payload 4. With the page endurance of about 10 k erase cycles quoted in
 * docs/peripherals-g474.md section 7, a small calibration record therefore
 * survives on the order of 10^6 updates. This is write amortisation rather
 * than wear spreading: the design deliberately reserves only one page, so
 * every erase still lands on the same page. A two-page ping-pong would spread
 * erase wear, at the cost of halving the record capacity and adding a
 * generation/sequence field.
 *
 * **Torn writes and CRC atomicity.** Flash programming is atomic per 64-bit
 * double-word, not per record. A reset in the middle of store_write() leaves
 * a prefix of the record programmed and the remainder reading 0xFF. The
 * header carries a CRC-32 over the payload and is programmed before most of
 * the payload (the CRC shares its double-word only with payload bytes 0..3).
 * A record is accepted only when the magic matches, the length is plausible,
 * the whole record fits in the page, and the CRC matches. A truncated record
 * therefore fails validation unless the missing bytes happen to produce the
 * same CRC, which has probability 2^-32. Scanning stops at the first invalid
 * record, and the last fully valid record - the previous calibration - remains
 * authoritative. If the invalid tail contains any non-0xFF byte, the next
 * store_write() erases the page before appending; programming over partially
 * cleared bits would otherwise raise PROGERR and could corrupt the new record.
 * That recovery erase discards the previous record as well, so a torn write
 * costs the last good calibration at the next update; the replacement record
 * then becomes the only one on the page. This is acceptable for a calibration
 * blob whose loss degrades to "uncalibrated", not for data that must survive
 * every reset.
 *
 * An erase is itself not atomic. Losing power during the erase can leave the
 * page partly erased, in which case no record (or only an older one below the
 * erase frontier) validates and the store reports "no calibration". That is a
 * loss of the stored values, never acceptance of corrupt values. The next
 * successful write performs a clean erase first.
 *
 * **Flash geometry (verified against the Cube G4 package and RM0440).**
 * The STM32G474CEU6 has 512 KB of main flash at 0x08000000. With the DBANK
 * option bit set (the factory default) the flash is two 256 KB banks of 2 KB
 * pages; with DBANK cleared it is one bank using 4 KB pages (`FLASH_PAGE_SIZE`
 * is 2 KB, `FLASH_PAGE_SIZE_128_BITS` is 4 KB in stm32g4xx_hal_flash.h).
 * 0x0807F800 is in the upper half of the address window. In dual-bank mode it
 * is the last 2 KB page of bank 2 (page 127 within that bank). In single-bank
 * mode page 127 is the last 4 KB page, 0x0807F000..0x0807FFFF, and the store
 * occupies its upper 2 KB. The page number (127) is therefore valid in both
 * layouts, but the bank passed to HAL_FLASHEx_Erase must be derived from the
 * address: bank 2 for an address in the upper half when DBANK is set, bank 1
 * otherwise. This assumes the default bank mapping (BFB2 = 0, no bank swap at
 * boot); enabling BFB2 would move the physical bank behind 0x0807F800 and the
 * address-derived bank would no longer match.
 *
 * @warning In single-bank (DBANK = 0) configuration the hardware erase
 *          granularity is 4 KB, so store_erase() also erases
 *          0x0807F000..0x0807F7FF. The linker script must reserve the whole
 *          top 4 KB in that configuration. The default dual-bank layout only
 *          touches 0x0807F800..0x0807FFFF.
 *
 * @warning store_write() and store_erase() are not reentrant and must not be
 *          called from an interrupt while another flash operation is in
 *          progress. The CRC table is built lazily on the first
 *          store_crc32() call without locking, so the first call must also not
 *          race an interrupt.
 *
 * The HAL identifiers used here were checked against
 * Drivers/STM32G4xx_HAL_Driver/Inc/stm32g4xx_hal_flash.h,
 * Inc/stm32g4xx_hal_flash_ex.h and Src/stm32g4xx_hal_flash_ex.c:
 *   - HAL_FLASH_Unlock() / HAL_FLASH_Lock()
 *   - HAL_FLASHEx_Erase() with FLASH_EraseInitTypeDef
 *   - FLASH_TYPEERASE_PAGES, FLASH_BANK_1, FLASH_BANK_2, NbPages
 *   - HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, address, uint64_t)
 */

#include "bsp_store.h"
#include "config.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Record layout and implementation limits                                   */
/* ------------------------------------------------------------------------- */

/** Size of one header in bytes (magic + version + len + crc). */
#define STORE_HDR_SIZE          12U

/** Size of the reserved store region, matching one 2 KB flash page. */
#define STORE_PAGE_SIZE         2048U

/**
 * Size of the static staging buffer. One complete record (header, payload and
 * 0xFF padding) is assembled here before the flash is touched. The buffer is
 * deliberately small: the store is meant for calibration blobs, not logs.
 */
#define STORE_SCRATCH_SIZE      512U

/**
 * Largest payload accepted by store_write().
 *
 * A record occupies align8(STORE_HDR_SIZE + len) bytes. With 512 bytes of
 * scratch the largest len whose padded record still fits is 500:
 * align8(12 + 500) = 512. A 501-byte payload would need 520 bytes and is
 * rejected with false. The format itself could hold more, but the fixed
 * scratch buffer (and the RAM budget of this bare-metal target) sets this
 * limit.
 */
#define STORE_MAX_PAYLOAD       (STORE_SCRATCH_SIZE - STORE_HDR_SIZE)

/** Round a byte count up to the 64-bit flash programming granularity. */
#define STORE_ALIGN8(x)         (((x) + 7U) & ~7U)

/** Value read from erased flash. */
#define STORE_ERASED_WORD       0xFFFFFFFFUL
#define STORE_ERASED_DW         0xFFFFFFFFFFFFFFFFULL

/** 12 bytes exactly; no compiler padding is permitted in the on-flash form. */
struct rec_hdr
{
    uint32_t magic;
    uint16_t version;
    uint16_t len;
    uint32_t crc;
};

/* Compile-time invariants. A failing check produces a negative array size. */
typedef char store_hdr_is_12_bytes[
    (sizeof(struct rec_hdr) == STORE_HDR_SIZE) ? 1 : -1];
typedef char store_scratch_is_granular[
    ((STORE_SCRATCH_SIZE % 8U) == 0U) ? 1 : -1];
typedef char store_page_is_granular[
    ((STORE_PAGE_SIZE % 8U) == 0U) ? 1 : -1];
typedef char store_region_matches_dual_bank_page[
    (STORE_PAGE_SIZE == FLASH_PAGE_SIZE) ? 1 : -1];
typedef char store_magic_is_not_erased[
    (STORE_RECORD_MAGIC != STORE_ERASED_WORD) ? 1 : -1];
typedef char store_addr_is_dw_aligned[
    ((STORE_FLASH_ADDR % 8U) == 0U) ? 1 : -1];
typedef char store_addr_is_page_aligned[
    ((STORE_FLASH_ADDR % STORE_PAGE_SIZE) == 0U) ? 1 : -1];

/* ------------------------------------------------------------------------- */
/* Driver state                                                              */
/* ------------------------------------------------------------------------- */

/**
 * @brief Cached result of the last page scan.
 *
 * All offsets are relative to STORE_FLASH_ADDR. Keeping only the newest valid
 * record means store_read() never has to rescan the page.
 */
struct store_state
{
    bool     initialized;    /**< store_init() completed successfully. */
    bool     has_record;     /**< newest_* describe a valid record. */
    bool     tail_dirty;     /**< bytes past append_off are not all 0xFF. */
    uint32_t append_off;     /**< next free, 8-byte aligned offset. */
    uint32_t newest_off;     /**< offset of the newest valid record. */
    uint16_t newest_len;     /**< payload length of that record. */
    uint16_t newest_version; /**< version of that record. */
    uint32_t write_count;    /**< valid records since the last erase. */
};

static struct store_state s_store;

/** Staging buffer for one padded record. */
static uint8_t s_scratch[STORE_SCRATCH_SIZE];

/** CRC-32 lookup table, built on first use. */
static uint32_t s_crc_table[256];
static bool s_crc_table_ready;

/* ------------------------------------------------------------------------- */
/* CRC-32                                                                    */
/* ------------------------------------------------------------------------- */

/** @brief Build the reflected CRC-32 table (poly 0xEDB88320). */
static void store_crc32_build_table(void)
{
    uint32_t i;
    uint32_t bit;
    uint32_t value;

    for (i = 0U; i < 256U; ++i)
    {
        value = i;
        for (bit = 0U; bit < 8U; ++bit)
        {
            if ((value & 1U) != 0U)
            {
                value = 0xEDB88320UL ^ (value >> 1U);
            }
            else
            {
                value >>= 1U;
            }
        }
        s_crc_table[i] = value;
    }

    s_crc_table_ready = true;
}

/**
 * @brief Standard reflected CRC-32 (IEEE 802.3).
 *
 * Polynomial 0xEDB88320, initial value 0xFFFFFFFF, final XOR 0xFFFFFFFF.
 * The check value for "123456789" is 0xCBF43926. The 256-entry table costs
 * 1 KB of RAM and is computed lazily on the first call; no allocation is
 * performed.
 *
 * @param data Source bytes. May be NULL only when len is zero.
 * @param len  Number of bytes.
 * @return CRC-32, or 0 for a NULL pointer with a non-zero length.
 */
uint32_t store_crc32(const void *data, uint32_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFUL;

    if ((bytes == NULL) && (len != 0U))
    {
        return 0U;
    }

    if (!s_crc_table_ready)
    {
        store_crc32_build_table();
    }

    while (len != 0U)
    {
        crc = s_crc_table[(crc ^ (uint32_t)(*bytes)) & 0xFFU] ^ (crc >> 8U);
        ++bytes;
        --len;
    }

    return crc ^ 0xFFFFFFFFUL;
}

/* ------------------------------------------------------------------------- */
/* Page geometry and low-level flash access                                  */
/* ------------------------------------------------------------------------- */

/**
 * @brief Translate the reserved page into the bank/page pair HAL expects.
 *
 * The bank is derived from the address instead of being hard-coded to
 * FLASH_BANK_2. 0x0807F800 is in the upper half of the 512 KB address window,
 * which is bank 2 when DBANK is set; if the option bytes ever select
 * single-bank mode the HAL ignores Banks, and page 127 is the final 4 KB page.
 * The page number itself stays STORE_FLASH_PAGE, because page numbering is
 * per-bank in dual-bank mode and absolute in single-bank mode, and both
 * layouts give page 127 for the top of flash.
 *
 * @param[out] banks FLASH_BANK_1 or FLASH_BANK_2.
 * @param[out] page  Page index to pass to HAL_FLASHEx_Erase().
 * @return true if the configured address really lies in that page.
 */
static bool store_page_geometry(uint32_t *banks, uint32_t *page)
{
    uint32_t flash_bytes = FLASH_SIZE;
    uint32_t page_size = FLASH_PAGE_SIZE;
    uint32_t page_base = 0U;
    uint32_t bank = FLASH_BANK_1;

    if ((banks == NULL) || (page == NULL))
    {
        return false;
    }

    if (flash_bytes < STORE_PAGE_SIZE)
    {
        return false;
    }

    if ((STORE_FLASH_ADDR < FLASH_BASE) ||
        (STORE_FLASH_ADDR >= (FLASH_BASE + flash_bytes)))
    {
        return false;
    }

    if (STORE_FLASH_PAGE >= FLASH_PAGE_NB)
    {
        return false;
    }

#if defined(FLASH_OPTR_DBANK)
    if ((FLASH->OPTR & FLASH_OPTR_DBANK) != 0U)
    {
        /* Dual bank: each bank is flash_bytes / 2, pages are 2 KB. */
        if (STORE_FLASH_ADDR >= (FLASH_BASE + (flash_bytes / 2U)))
        {
            bank = FLASH_BANK_2;
            page_base = FLASH_BASE + (flash_bytes / 2U);
        }
        else
        {
            bank = FLASH_BANK_1;
            page_base = FLASH_BASE;
        }

        page_base += (STORE_FLASH_PAGE * page_size);

        /* In dual-bank mode the reserved 2 KB region is exactly the page. */
        if (page_base != STORE_FLASH_ADDR)
        {
            return false;
        }
    }
    else
    {
        /* Single bank: the 512 KB part uses 128-bit reads and 4 KB pages.
         * The store is the upper half of the final 4 KB page. */
        bank = FLASH_BANK_1;
        page_size = FLASH_PAGE_SIZE_128_BITS;
        page_base = FLASH_BASE + (STORE_FLASH_PAGE * page_size);

        if (STORE_FLASH_ADDR < page_base)
        {
            return false;
        }
        if ((STORE_FLASH_ADDR + STORE_PAGE_SIZE) > (page_base + page_size))
        {
            return false;
        }
    }
#else
    /* No DBANK option implemented on this die: one bank, 2 KB pages. */
    bank = FLASH_BANK_1;
    page_base = FLASH_BASE + (STORE_FLASH_PAGE * page_size);

    if (page_base != STORE_FLASH_ADDR)
    {
        return false;
    }
#endif

    /* Never allow the computed page to run past main flash. */
    if ((page_base + page_size) > (FLASH_BASE + flash_bytes))
    {
        return false;
    }

    *banks = bank;
    *page = STORE_FLASH_PAGE;
    return true;
}

/**
 * @brief Check that the configured address is usable before touching flash.
 * @return true if the page geometry and alignment are consistent.
 */
static bool store_layout_valid(void)
{
    uint32_t banks;
    uint32_t page;

    if ((STORE_FLASH_ADDR % 8U) != 0U)
    {
        return false;
    }

    if ((STORE_FLASH_ADDR % STORE_PAGE_SIZE) != 0U)
    {
        return false;
    }

    if ((STORE_FLASH_ADDR + STORE_PAGE_SIZE) >
        (FLASH_BASE + FLASH_SIZE))
    {
        return false;
    }

    return store_page_geometry(&banks, &page);
}

/**
 * @brief Program one double-word inside the reserved page.
 *
 * This is the only function that calls HAL_FLASH_Program(). It refuses any
 * address that is not an 8-byte-aligned double-word wholly inside
 * [STORE_FLASH_ADDR, STORE_FLASH_ADDR + 2048). The bounds check is the
 * in-code guard against ever programming application flash.
 *
 * @note The caller must have unlocked the flash controller.
 *
 * @param offset Byte offset relative to STORE_FLASH_ADDR (multiple of 8).
 * @param data   Double-word to program.
 * @return true on HAL success.
 */
static bool store_program_doubleword(uint32_t offset, uint64_t data)
{
    uint32_t address;

    if ((offset % 8U) != 0U)
    {
        return false;
    }

    if (offset > (STORE_PAGE_SIZE - 8U))
    {
        return false;
    }

    address = STORE_FLASH_ADDR + offset;

    if ((address < STORE_FLASH_ADDR) ||
        ((address + 8U) > (STORE_FLASH_ADDR + STORE_PAGE_SIZE)))
    {
        return false;
    }

    return HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, address, data) == HAL_OK;
}

/**
 * @brief Erase the one reserved page.
 *
 * Uses FLASH_TYPEERASE_PAGES with the bank derived from the address and
 * Page = STORE_FLASH_PAGE, NbPages = 1. The flash is unlocked for the
 * operation and always locked again, including on failure.
 *
 * @return true if HAL reported success and no page was flagged bad.
 */
static bool store_erase_page(void)
{
    FLASH_EraseInitTypeDef erase;
    uint32_t banks = FLASH_BANK_1;
    uint32_t page = 0U;
    uint32_t page_error = 0xFFFFFFFFUL;
    HAL_StatusTypeDef status;

    if (!store_page_geometry(&banks, &page))
    {
        return false;
    }

    if (HAL_FLASH_Unlock() != HAL_OK)
    {
        return false;
    }

    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);

    erase.TypeErase = FLASH_TYPEERASE_PAGES;
    erase.Banks = banks;
    erase.Page = page;
    erase.NbPages = 1U;

    status = HAL_FLASHEx_Erase(&erase, &page_error);

    (void)HAL_FLASH_Lock();

    return (status == HAL_OK) && (page_error == 0xFFFFFFFFUL);
}

/* ------------------------------------------------------------------------- */
/* Page scanning                                                             */
/* ------------------------------------------------------------------------- */

/** @brief Copy a record header out of flash without unaligned accesses. */
static void store_read_header(uint32_t offset, struct rec_hdr *hdr)
{
    memcpy(hdr, (const void *)(STORE_FLASH_ADDR + offset), sizeof(*hdr));
}

/**
 * @brief Check whether the region from @p offset to the end of the page is
 *        still erased (all 0xFF).
 *
 * A false result means a torn or abandoned record sits at @p offset. The next
 * append must erase the page rather than program over bits that may already
 * have been cleared.
 */
static bool store_tail_erased(uint32_t offset)
{
    const uint8_t *flash = (const uint8_t *)STORE_FLASH_ADDR;
    uint32_t i;

    if (offset > STORE_PAGE_SIZE)
    {
        return false;
    }

    for (i = offset; i < STORE_PAGE_SIZE; ++i)
    {
        if (flash[i] != 0xFFU)
        {
            return false;
        }
    }

    return true;
}

/** @brief Clear the cached scan result (does not touch flash). */
static void store_state_reset(void)
{
    s_store.has_record = false;
    s_store.tail_dirty = false;
    s_store.append_off = 0U;
    s_store.newest_off = 0U;
    s_store.newest_len = 0U;
    s_store.newest_version = 0U;
    s_store.write_count = 0U;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

/**
 * @brief Scan the page and locate the newest valid record.
 *
 * Validation per record:
 *   - magic equals STORE_RECORD_MAGIC (0xFFFFFFFF means erased tail);
 *   - len is at most STORE_MAX_PAYLOAD (500 bytes);
 *   - header plus padded payload fits before the end of the page;
 *   - CRC-32 over the payload matches the stored crc.
 *
 * Scanning stops at the first record that fails validation. That is safe
 * because records are appended strictly in address order: no valid record can
 * follow an invalid one produced by a torn write. The append pointer is left
 * at the start of the invalid tail so store_write() can erase and reuse it.
 *
 * The page is only read here; no unlock or erase happens at boot.
 *
 * @return true if the page geometry is usable (even if it is empty).
 */
bool store_init(void)
{
    struct rec_hdr hdr;
    uint32_t offset = 0U;
    uint32_t rec_size;
    uint32_t crc;

    s_store.initialized = false;
    store_state_reset();

    if (!store_layout_valid())
    {
        return false;
    }

    while (offset < STORE_PAGE_SIZE)
    {
        if ((STORE_PAGE_SIZE - offset) < STORE_HDR_SIZE)
        {
            break;
        }

        store_read_header(offset, &hdr);

        if (hdr.magic == STORE_ERASED_WORD)
        {
            /* First erased double-word: end of the log. */
            break;
        }

        if (hdr.magic != STORE_RECORD_MAGIC)
        {
            break;
        }

        if (hdr.len > STORE_MAX_PAYLOAD)
        {
            break;
        }

        rec_size = STORE_ALIGN8(STORE_HDR_SIZE + (uint32_t)hdr.len);

        if (rec_size > (STORE_PAGE_SIZE - offset))
        {
            break;
        }

        crc = store_crc32(
            (const void *)(STORE_FLASH_ADDR + offset + STORE_HDR_SIZE),
            (uint32_t)hdr.len);

        if (crc != hdr.crc)
        {
            break;
        }

        s_store.newest_off = offset;
        s_store.newest_len = hdr.len;
        s_store.newest_version = hdr.version;
        s_store.has_record = true;
        ++s_store.write_count;

        offset += rec_size;
    }

    s_store.append_off = offset;
    s_store.tail_dirty = !store_tail_erased(offset);
    s_store.initialized = true;

    return true;
}

/**
 * @brief Read the newest valid payload.
 *
 * The payload is copied from flash directly (a normal memory read; no unlock
 * is needed). It is clipped to the capacity supplied in *len. On return *len
 * is the record's true payload length, which may be larger than the capacity
 * if the caller's buffer was too small; only min(capacity, true length) bytes
 * are written to @p out, so the caller can detect truncation by comparing
 * *len with the capacity it passed in. *version receives the record version.
 *
 * @param out     Destination buffer (may be NULL only when capacity is 0).
 * @param len     In: capacity in bytes. Out: true payload length.
 * @param version Out: record version.
 * @return false if the store holds no valid record.
 */
bool store_read(void *out, uint16_t *len, uint16_t *version)
{
    uint32_t capacity;
    uint32_t copy_len;

    if ((len == NULL) || (version == NULL))
    {
        return false;
    }

    if (!s_store.initialized)
    {
        if (!store_init())
        {
            *len = 0U;
            *version = 0U;
            return false;
        }
    }

    if (!s_store.has_record)
    {
        *len = 0U;
        *version = 0U;
        return false;
    }

    capacity = (uint32_t)(*len);
    copy_len = (uint32_t)s_store.newest_len;

    if (copy_len > capacity)
    {
        copy_len = capacity;
    }

    if ((copy_len != 0U) && (out == NULL))
    {
        return false;
    }

    if (copy_len != 0U)
    {
        memcpy(out,
               (const void *)(STORE_FLASH_ADDR + s_store.newest_off + STORE_HDR_SIZE),
               copy_len);
    }

    *len = s_store.newest_len;
    *version = s_store.newest_version;

    return true;
}

/**
 * @brief Append a new record, erasing the page first if necessary.
 *
 * The complete padded record is assembled in the 512-byte scratch buffer
 * before flash is touched. This also makes it safe for @p data to point into
 * the store page itself: the bytes are copied before a possible erase.
 *
 * If the record does not fit, or the previous scan found a non-erased torn
 * tail, exactly one page erase is performed and writing restarts at offset 0.
 * The page is unlocked once for the whole record, each double-word is
 * programmed with HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, ...), and
 * the lock is restored unconditionally. Any programming failure returns false
 * and marks the tail dirty so the next attempt starts with a clean erase.
 *
 * Records larger than STORE_MAX_PAYLOAD (500 bytes, because the padded record
 * must fit the 512-byte scratch) are rejected.
 *
 * @param data    Payload bytes. May be NULL only when len is zero.
 * @param len     Payload length, 0 ... 500.
 * @param version Caller-defined record version.
 * @return false on invalid arguments, geometry failure, or flash failure.
 */
bool store_write(const void *data, uint16_t len, uint16_t version)
{
    struct rec_hdr hdr;
    uint32_t rec_size;
    uint32_t offset;
    uint32_t i;
    uint64_t doubleword;
    bool erase_first;

    if ((data == NULL) && (len != 0U))
    {
        return false;
    }

    if ((uint32_t)len > STORE_MAX_PAYLOAD)
    {
        return false;
    }

    if (!s_store.initialized)
    {
        if (!store_init())
        {
            return false;
        }
    }

    rec_size = STORE_ALIGN8(STORE_HDR_SIZE + (uint32_t)len);

    if (rec_size > STORE_SCRATCH_SIZE)
    {
        return false;
    }

    /* Build the exact on-flash image first. Padding stays 0xFF, the erased
     * value, so a skipped padding double-word needs no programming. */
    memset(s_scratch, 0xFF, sizeof(s_scratch));

    hdr.magic = STORE_RECORD_MAGIC;
    hdr.version = version;
    hdr.len = len;
    hdr.crc = store_crc32(data, (uint32_t)len);

    memcpy(s_scratch, &hdr, STORE_HDR_SIZE);

    if (len != 0U)
    {
        memcpy(&s_scratch[STORE_HDR_SIZE], data, (uint32_t)len);
    }

    erase_first = s_store.tail_dirty ||
                  (s_store.append_off > (STORE_PAGE_SIZE - rec_size));

    if (erase_first)
    {
        if (!store_erase_page())
        {
            /* The page may be partially erased; force a fresh scan before the
             * next read or write rather than trusting the cached record. */
            s_store.initialized = false;
            return false;
        }

        store_state_reset();
        s_store.initialized = true;
    }

    if (HAL_FLASH_Unlock() != HAL_OK)
    {
        return false;
    }

    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);

    for (i = 0U; i < rec_size; i += 8U)
    {
        offset = s_store.append_off + i;

        doubleword = 0U;
        memcpy(&doubleword, &s_scratch[i], sizeof(doubleword));

        /* Pure padding is already 0xFF in erased flash. Skipping it avoids
         * pointless programming and any doubt about all-ones words. */
        if (doubleword == STORE_ERASED_DW)
        {
            continue;
        }

        if (!store_program_doubleword(offset, doubleword))
        {
            (void)HAL_FLASH_Lock();
            s_store.tail_dirty = true;
            return false;
        }
    }

    (void)HAL_FLASH_Lock();

    s_store.newest_off = s_store.append_off;
    s_store.newest_len = len;
    s_store.newest_version = version;
    s_store.has_record = true;
    s_store.append_off += rec_size;
    ++s_store.write_count;
    s_store.tail_dirty = false;

    return true;
}

/**
 * @brief Erase the whole store.
 *
 * Uses HAL_FLASHEx_Erase() with FLASH_TYPEERASE_PAGES, the address-derived
 * bank, Page = STORE_FLASH_PAGE and NbPages = 1. The flash is locked again on
 * every path. The cached scan state is cleared on success.
 *
 * @return false if the geometry check, unlock, or erase failed.
 */
bool store_erase(void)
{
    if (!store_erase_page())
    {
        /* A failed erase may have left the page partly erased. Drop the cache
         * so the next access rescans the real flash contents. */
        s_store.initialized = false;
        return false;
    }

    store_state_reset();
    s_store.initialized = true;

    return true;
}

/**
 * @brief Number of valid records appended since the last page erase.
 *
 * This is a wear diagnostic. It is reconstructed by store_init() on boot
 * (valid records still on the page) and reset to zero by store_erase(). A
 * failed or torn attempt is not counted because it never became a record.
 */
uint32_t store_write_count(void)
{
    return s_store.write_count;
}
