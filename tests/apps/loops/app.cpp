// E1 regression app: many short inner loops (GCC emits SPLOOP for the bit loop) while audio interrupts arrive.
// An interrupt that corrupted ILC/RILC (the loop counters) would hang or change the result. Logs the CRC-32
// of a 256 KiB pattern; tests/test_e1.py compares it with Python's. Also allocates 100 MiB from the heap.
#include <enaga/app.hpp>

#include <cstdint>

namespace {
uint8_t g_buffer[256 * 1024];

uint32_t crc32(const uint8_t *data, uint32_t bytes) {
    uint32_t crc = 0xFFFFFFFFu;
    while (bytes--) {
        crc ^= *data++;
        for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
}  // namespace

class Loops : public enaga::App {
public:
    bool init() override {
        for (uint32_t i = 0; i < sizeof g_buffer; i++) g_buffer[i] = uint8_t(i * 7 + (i >> 9));
        uint32_t crc = crc32(g_buffer, sizeof g_buffer);
        char text[16] = "CRC ";
        for (int i = 0; i < 8; i++) text[4 + i] = "0123456789ABCDEF"[(crc >> (28 - 4 * i)) & 15];
        text[12] = 0;
        enaga::log(text);
        // The heap runs from the end of this image to the end of DDR: a 100 MiB block must fit.
        constexpr uint32_t kBig = 100u * 1024 * 1024;
        uint8_t *big = new uint8_t[kBig];
        big[0] = 0x5A;
        big[kBig - 1] = 0xA5;
        enaga::log(big[0] == 0x5A && big[kBig - 1] == 0xA5 ? "BIG OK" : "BIG BAD");
        delete[] big;
        return true;
    }
};

ENAGA_APP(Loops)
