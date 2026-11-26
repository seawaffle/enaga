/* Enaga package loader. Freestanding C, shared by the firmware and the test harness.
 *
 * Two-step use, so the firmware can read the payload from the card straight into the app window:
 *   1. enaga_package_check(header, file_size) validates the header against the app window;
 *   2. the caller places load_size payload bytes at header->load_address;
 *   3. enaga_package_start(header, host, cache_sync, &app) checks the CRC in place, zeroes .bss, syncs
 *      caches (writeback + invalidate, so fetches see the new code), and calls the entry.
 * enaga_load_image does all three for a package already in memory.
 */
#ifndef ENAGA_LOADER_H
#define ENAGA_LOADER_H

#include <enaga/abi.h>

typedef enum enaga_load_status {
    ENAGA_LOAD_OK = 0,
    ENAGA_LOAD_TRUNCATED = -1,      /* file shorter than the header, or the payload or icon it declares */
    ENAGA_LOAD_BAD_MAGIC = -2,
    ENAGA_LOAD_BAD_HEADER = -3,     /* header_size or flags not understood */
    ENAGA_LOAD_ABI = -4,            /* built for another ABI version */
    ENAGA_LOAD_WINDOW = -5,         /* load address, size or entry outside the app window */
    ENAGA_LOAD_CRC = -6,
    ENAGA_LOAD_ENTRY_FAILED = -7,   /* the entry returned no table, or a table for another ABI */
} enaga_load_status_t;

typedef void (*enaga_cache_sync_t)(uint32_t address, uint32_t bytes);

enaga_load_status_t enaga_package_check(const enaga_package_header_t *header, uint32_t file_size);
enaga_load_status_t enaga_package_start(const enaga_package_header_t *header, const enaga_host_t *host,
                                        enaga_cache_sync_t cache_sync, const enaga_app_t **app);
enaga_load_status_t enaga_load_image(const uint8_t *package, uint32_t size, const enaga_host_t *host,
                                     enaga_cache_sync_t cache_sync, const enaga_app_t **app);
uint32_t enaga_crc32(const uint8_t *data, uint32_t bytes);

/* Called every ENAGA_LOADER_STEP bytes of the CRC and .bss clearing when set (the firmware feeds its watchdog:
 * a large package takes seconds). */
#define ENAGA_LOADER_STEP 0x10000u
extern void (*enaga_loader_progress)(void);

#endif
