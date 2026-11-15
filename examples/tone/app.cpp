// Tone: an example app. STEP 1-8 play a sine at 750 Hz x step (STEP 1 = 750 Hz, exactly 64 frames per
// cycle); the same STEP again stops it. Encoder VALUE changes the level. Shows the note and the audio load.
//
// The sine comes from a 64-entry table built at init by rotating a unit vector (no libm), read with linear
// interpolation by a phase accumulator in the audio callback. State shared between the main and audio contexts
// is one word each, written by one side only.
#include <enaga/app.hpp>

#include <array>
#include <cstdint>

namespace {

constexpr int kTable = 64;

void append_number(char *&out, unsigned value) {
    char digits[12];
    int n = 0;
    do { digits[n++] = char('0' + value % 10); value /= 10; } while (value);
    while (n) *out++ = digits[--n];
}

class Tone : public enaga::App {
public:
    bool init() override {
        // cos/sin of 2 pi / 64 to float precision, then 64 rotations.
        const float c = 0.99518472667f, s = 0.09801714033f;
        float x = 1.0f, y = 0.0f;
        for (int i = 0; i < kTable; i++) {
            table_[i] = y;
            float nx = x * c - y * s;
            y = x * s + y * c;
            x = nx;
        }
        table_[kTable] = table_[0];
        enaga::log("tone ready");
        return true;
    }

    void on_key(int key, bool down) override {
        if (!down || key < ENAGA_KEY_STEP1 || key >= ENAGA_KEY_STEP1 + 8) return;
        uint32_t step = uint32_t(key - ENAGA_KEY_STEP1 + 1);
        step_ = step_ == step ? 0 : step;
        // Phase increment in table entries per frame, 16.16 fixed point: 750 Hz x step at 48 kHz.
        increment_ = step_ * (uint32_t(kTable) << 16) / 64u;
        enaga::log(step_ ? "note on" : "note off");
    }

    void on_encoder(int encoder, int delta) override {
        if (encoder != 4) return;
        int level = int(level_percent_) + delta * 5;
        level_percent_ = uint32_t(level < 0 ? 0 : level > 100 ? 100 : level);
    }

    void audio(const float *, float *out, uint32_t frames) override {
        const uint32_t increment = increment_;
        const float level = float(level_percent_) * 0.01f;
        for (uint32_t i = 0; i < frames; i++) {
            float sample = 0.0f;
            if (increment) {
                uint32_t index = phase_ >> 16;
                float frac = float(phase_ & 0xFFFF) * (1.0f / 65536.0f);
                sample = level * (table_[index] + (table_[index + 1] - table_[index]) * frac);
                phase_ = (phase_ + increment) & ((uint32_t(kTable) << 16) - 1);
            } else {
                phase_ = 0;
            }
            out[2 * i] = sample;
            out[2 * i + 1] = sample;
        }
    }

    void draw(enaga::Canvas &canvas) override {
        char line[32], *out = line;
        canvas.clear();
        canvas.text(0, 0, "TONE");
        if (step_) {
            append_number(out, 750 * step_);
            *out++ = ' '; *out++ = 'H'; *out++ = 'Z';
        } else {
            for (const char *t = "STEP 1-8"; *t; t++) *out++ = *t;
        }
        *out = 0;
        canvas.text(0, 20, line);
        out = line;
        for (const char *t = "LEVEL "; *t; t++) *out++ = *t;
        append_number(out, level_percent_);
        *out = 0;
        canvas.text(0, 32, line, 10);
        out = line;
        for (const char *t = "LOAD "; *t; t++) *out++ = *t;
        append_number(out, enaga::audio_load_peak());
        *out = 0;
        canvas.text(0, 44, line, 10);
        canvas.rect(0, 60, int(level_percent_), 4, 15);
    }

private:
    std::array<float, kTable + 1> table_{};
    volatile uint32_t step_ = 0;
    volatile uint32_t increment_ = 0;      // written by the main context, read by audio
    volatile uint32_t level_percent_ = 25;
    uint32_t phase_ = 0;                   // audio context only
};

}  // namespace

ENAGA_APP(Tone)
