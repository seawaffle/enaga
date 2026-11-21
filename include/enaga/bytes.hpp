// Enaga byte helpers: numbers in packed data (file formats, SysEx, card data) at any address.
//
// On the C6748 a 16-, 32- or 64-bit load or store at a misaligned address does not fault: it silently uses the
// aligned address below, so `*(const uint32_t *)(p + 1)` reads the wrong bytes and a store there overwrites the
// word below. These go byte by byte, so they work at any address. Little-endian is the processor's own order
// (WAV, most binary formats); big-endian is for formats such as standard MIDI files and AIFF.
//
//   uint32_t size = enaga::bytes::load_le32(header + 4);
//   enaga::bytes::store_be16(track + 2, tempo);
#pragma once

#include <stdint.h>

namespace enaga::bytes {

inline uint16_t load_le16(const void *at) {
    const uint8_t *p = static_cast<const uint8_t *>(at);
    return uint16_t(p[0] | p[1] << 8);
}
inline uint32_t load_le32(const void *at) {
    const uint8_t *p = static_cast<const uint8_t *>(at);
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline uint16_t load_be16(const void *at) {
    const uint8_t *p = static_cast<const uint8_t *>(at);
    return uint16_t(p[0] << 8 | p[1]);
}
inline uint32_t load_be32(const void *at) {
    const uint8_t *p = static_cast<const uint8_t *>(at);
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

inline void store_le16(void *at, uint16_t v) {
    uint8_t *p = static_cast<uint8_t *>(at);
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8);
}
inline void store_le32(void *at, uint32_t v) {
    uint8_t *p = static_cast<uint8_t *>(at);
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
}
inline void store_be16(void *at, uint16_t v) {
    uint8_t *p = static_cast<uint8_t *>(at);
    p[0] = uint8_t(v >> 8); p[1] = uint8_t(v);
}
inline void store_be32(void *at, uint32_t v) {
    uint8_t *p = static_cast<uint8_t *>(at);
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

}  // namespace enaga::bytes
