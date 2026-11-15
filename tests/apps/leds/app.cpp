// LED test app. STEP 1 lights the next LED of the walk (red 1, green 2, blue 3) and switches the previous one
// off; past the end it lights nothing. STEP 2: pad 0 at red 15 (clamped to 10). STEP 3: the audio callback
// tries to light pad 1 (ignored). STEP 4: fatal(), so Enaga drops the app and switches every LED off.
#include <enaga/app.hpp>

namespace {
constexpr int kButtons[] = {ENAGA_KEY_HOME, ENAGA_KEY_MIXER, ENAGA_KEY_REC, ENAGA_KEY_PROJECT, ENAGA_KEY_EDIT,
                            ENAGA_KEY_FUNC, ENAGA_KEY_STOP, ENAGA_KEY_PLAY, ENAGA_KEY_CLR, ENAGA_KEY_OK,
                            ENAGA_KEY_OCTAVE_LEFT};   // the last has no LED
constexpr int kWalk = 15 + 4 + 16 + 11;

void light(int i, unsigned r, unsigned g, unsigned b) {
    if (i < 15) enaga::pad_led(i, r, g, b);
    else if (i < 17) enaga::vol_led(i - 15, b);
    else if (i == 17) enaga::mic_led(b);
    else if (i == 18) enaga::value_led(r, g, b);
    else if (i < 35) enaga::step_led(i - 18, r, g);
    else enaga::button_led(kButtons[i - 35], r, g);
}
}

class Leds : public enaga::App {
public:
    void on_key(int key, bool down) override {
        if (!down) return;
        if (key == ENAGA_KEY_STEP1) {
            if (next_ > 0 && next_ <= kWalk) light(next_ - 1, 0, 0, 0);
            if (next_ < kWalk) light(next_, 1, 2, 3);
            next_++;
        }
        if (key == ENAGA_KEY_STEP1 + 1) enaga::pad_led(0, 15, 0, 0);
        if (key == ENAGA_KEY_STEP1 + 2) audio_led_ = true;
        if (key == ENAGA_KEY_STEP1 + 3) enaga::fatal("leds done");
    }
    void audio(const float *, float *out, uint32_t frames) override {
        if (audio_led_) enaga::pad_led(1, 5, 5, 5);
        for (uint32_t i = 0; i < 2 * frames; i++) out[i] = 0.0f;
    }

private:
    int next_ = 0;
    volatile bool audio_led_ = false;
};

ENAGA_APP(Leds)
