/* Enaga package loader. See loader.h. */
#include "loader.h"

void (*enaga_loader_progress)(void);

uint32_t enaga_crc32(const uint8_t *data, uint32_t bytes) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < bytes; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        if ((i & (ENAGA_LOADER_STEP - 1u)) == ENAGA_LOADER_STEP - 1u && enaga_loader_progress) enaga_loader_progress();
    }
    return ~crc;
}

enaga_load_status_t enaga_package_check(const enaga_package_header_t *header, uint32_t file_size) {
    if (file_size < sizeof(*header)) return ENAGA_LOAD_TRUNCATED;
    if (header->magic != ENAGA_PACKAGE_MAGIC) return ENAGA_LOAD_BAD_MAGIC;
    if (header->header_size != sizeof(*header) || (header->flags & ~ENAGA_PACKAGE_ICON)) return ENAGA_LOAD_BAD_HEADER;
    if (header->abi_version != ENAGA_ABI_VERSION) return ENAGA_LOAD_ABI;   /* exact match until 1.0 */
    uint32_t window = ENAGA_APP_LIMIT - ENAGA_APP_BASE;
    if (header->load_address != ENAGA_APP_BASE || header->load_size == 0 || header->load_size > window ||
        header->bss_size > window - header->load_size || header->entry_offset >= header->load_size ||
        (header->entry_offset & 3u))
        return ENAGA_LOAD_WINDOW;
    if (file_size - sizeof(*header) < header->load_size) return ENAGA_LOAD_TRUNCATED;
    if ((header->flags & ENAGA_PACKAGE_ICON) && file_size - sizeof(*header) - header->load_size < ENAGA_ICON_BYTES)
        return ENAGA_LOAD_TRUNCATED;
    return ENAGA_LOAD_OK;
}

enaga_load_status_t enaga_package_start(const enaga_package_header_t *header, const enaga_host_t *host,
                                        enaga_cache_sync_t cache_sync, const enaga_app_t **app) {
    uint8_t *base = (uint8_t *)(uintptr_t)header->load_address;
    *app = 0;
    if (enaga_crc32(base, header->load_size) != header->crc32) return ENAGA_LOAD_CRC;
    for (uint32_t i = 0; i < header->bss_size; i++) {
        base[header->load_size + i] = 0;
        if ((i & (ENAGA_LOADER_STEP - 1u)) == ENAGA_LOADER_STEP - 1u && enaga_loader_progress) enaga_loader_progress();
    }
    if (cache_sync) cache_sync(header->load_address, header->load_size + header->bss_size);
    enaga_entry_t entry = (enaga_entry_t)(uintptr_t)(header->load_address + header->entry_offset);
    const enaga_app_t *table = entry(host);
    if (!table || table->abi_version != ENAGA_ABI_VERSION || table->size < sizeof(enaga_app_t))
        return ENAGA_LOAD_ENTRY_FAILED;
    *app = table;
    return ENAGA_LOAD_OK;
}

enaga_load_status_t enaga_load_image(const uint8_t *package, uint32_t size, const enaga_host_t *host,
                                     enaga_cache_sync_t cache_sync, const enaga_app_t **app) {
    const enaga_package_header_t *header = (const enaga_package_header_t *)package;
    *app = 0;
    enaga_load_status_t status = enaga_package_check(header, size);
    if (status != ENAGA_LOAD_OK) return status;
    uint8_t *destination = (uint8_t *)(uintptr_t)header->load_address;
    const uint8_t *payload = package + header->header_size;
    for (uint32_t i = 0; i < header->load_size; i++) destination[i] = payload[i];
    return enaga_package_start(header, host, cache_sync, app);
}
