// Enaga: WAV headers. parse() finds the format and the sample data in the first bytes of a file; header() makes
// the 44 bytes that start a PCM or float file, for a recording (Stream::create reserves them, Stream::close
// writes them with the final size).
//
//     uint8_t head[512];                                   // read from the file's start (File::read, read_all)
//     enaga::wav::Format wav;
//     if (enaga::wav::parse(head, got, wav) && wav.encoding == enaga::wav::pcm && wav.bits == 16) ...
//
// The fields are read byte by byte: WAV is packed, and the C674x silently rounds a misaligned word access down.
#pragma once

#include <cstdint>

namespace enaga::wav {

enum Encoding : uint16_t { pcm = 1, floating = 3 };

struct Format {
    uint16_t encoding = pcm;       // pcm (8-bit unsigned, 16/24/32-bit signed) or floating (32-bit)
    uint16_t channels = 2;
    uint32_t rate = 48000;
    uint16_t bits = 16;
    uint32_t data_offset = 0;      // where the samples start in the file
    uint32_t data_bytes = 0;       // how many bytes of them (a header still being written may say 0)
    uint32_t frame_bytes() const { return uint32_t(channels) * (bits / 8u); }
};

namespace detail {
inline uint32_t u16(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
inline uint32_t u32(const uint8_t *p) { return u16(p) | u16(p + 2) << 16; }
inline void put16(uint8_t *p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
inline bool tag(const uint8_t *p, const char *t) { return p[0] == t[0] && p[1] == t[1] && p[2] == t[2] && p[3] == t[3]; }
}  // namespace detail

// True for a RIFF WAVE whose "fmt " chunk and the start of its "data" chunk lie in the `bytes` given, in a
// format listed above (WAVE_FORMAT_EXTENSIBLE included, by its sub-format).
inline bool parse(const void *start, uint32_t bytes, Format &out) {
    using namespace detail;
    const uint8_t *p = static_cast<const uint8_t *>(start);
    if (bytes < 12 || !tag(p, "RIFF") || !tag(p + 8, "WAVE")) return false;
    bool have_format = false;
    for (uint32_t at = 12; at + 8 <= bytes;) {
        uint32_t size = u32(p + at + 4), body = at + 8;
        if (tag(p + at, "fmt ")) {
            if (size < 16 || body + 16 > bytes) return false;
            out.encoding = uint16_t(u16(p + body));
            if (out.encoding == 0xFFFE && size >= 40 && body + 26 <= bytes) out.encoding = uint16_t(u16(p + body + 24));
            out.channels = uint16_t(u16(p + body + 2));
            out.rate = u32(p + body + 4);
            out.bits = uint16_t(u16(p + body + 14));
            have_format = true;
        } else if (tag(p + at, "data")) {
            if (!have_format) return false;
            out.data_offset = body;
            out.data_bytes = size;
            bool known = out.encoding == pcm ? out.bits == 8 || out.bits == 16 || out.bits == 24 || out.bits == 32
                                             : out.encoding == floating && out.bits == 32;
            return known && out.channels > 0;
        }
        if (size > 0xFFFFFFFFu - body - 1) return false;
        at = body + size + (size & 1);            // chunks are padded to even sizes
    }
    return false;
}

// The 44-byte header of a file holding `data_bytes` of samples in `format` (data_offset is ignored: 44).
inline void header(uint8_t out[44], const Format &format, uint32_t data_bytes) {
    using namespace detail;
    const char *riff = "RIFF", *wave = "WAVEfmt ", *data = "data";
    for (int i = 0; i < 4; i++) out[i] = uint8_t(riff[i]);
    put32(out + 4, 36 + data_bytes);
    for (int i = 0; i < 8; i++) out[8 + i] = uint8_t(wave[i]);
    put32(out + 16, 16);
    put16(out + 20, format.encoding);
    put16(out + 22, format.channels);
    put32(out + 24, format.rate);
    put32(out + 28, format.rate * format.frame_bytes());
    put16(out + 32, format.frame_bytes());
    put16(out + 34, format.bits);
    for (int i = 0; i < 4; i++) out[36 + i] = uint8_t(data[i]);
    put32(out + 40, data_bytes);
}

}  // namespace enaga::wav
