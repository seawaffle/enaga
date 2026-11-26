// Output conversion test app: every block is the same 128 words, each written as a float bit pattern: first the
// edge cases (zero, +-1 and just inside and past it, the smallest steps, denormals, infinities, NaNs), then
// pseudo-random 32-bit patterns, which land in every exponent. test_e1 checks each transmitted word against
// x * 2^31 truncated, clipped to full scale, NaN silent.
#include <enaga/app.hpp>

namespace {
const uint32_t kEdges[] = {
    0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x3F7FFFFF, 0xBF7FFFFF, 0x3F800001, 0xBF800001,
    0x3F000000, 0xBF000000, 0x30000000, 0xB0000000, 0x30400000, 0x2FFFFFFF, 0x00000001, 0x807FFFFF,
    0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000, 0x7F800001, 0x7F7FFFFF, 0xFF7FFFFF, 0x40000000,
};
}  // namespace

class Clip : public enaga::App {
public:
    bool init() override { enaga::log("clip ready"); return true; }
    void audio(const float *, float *out, uint32_t frames) override {
        uint32_t s = 1;
        for (uint32_t i = 0; i < 2 * frames; i++) {
            union { uint32_t u; float f; } w;
            s = s * 1664525u + 1013904223u;
            w.u = i < sizeof kEdges / sizeof kEdges[0] ? kEdges[i] : s;
            out[i] = w.f;
        }
    }
};

ENAGA_APP(Clip)
