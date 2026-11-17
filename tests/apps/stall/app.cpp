// Audio ring test app: every block is one constant level, the block's number. STEP 1 / STEP 2: the next
// callback spins for about 2.5 / 3.5 blocks, so the ring finishes two / three more blocks before the interrupt
// returns and their completions merge in the DMA's pending bits (kernel/audio.c).
#include <enaga/app.hpp>

class Stall : public enaga::App {
public:
    bool init() override { enaga::log("stall ready"); return true; }
    void on_key(int key, bool down) override {
        if (down && key == ENAGA_KEY_STEP1) spin_ = 95000;
        if (down && key == ENAGA_KEY_STEP1 + 1) spin_ = 133000;
    }
    void audio(const float *, float *out, uint32_t frames) override {
        if (spin_) {
            for (uint32_t n = 0; n < spin_; n++) sink_ = n;
            spin_ = 0;
        }
        block_++;
        for (uint32_t i = 0; i < 2 * frames; i++) out[i] = (float)block_ / 65536.0f;
    }

private:
    volatile uint32_t spin_ = 0, sink_ = 0;
    uint32_t block_ = 0;
};

ENAGA_APP(Stall)
