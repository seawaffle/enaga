// E1 failure-path app: plays a constant level so its audio is recognisable, then fails on request.
// STEP 1: fatal() from update (main context). STEP 2: fatal() from the audio callback (interrupt context).
// STEP 3 / 4: a CPU exception (SWE, software exception) from update / from the audio callback.
// STEP 5 / 6: never return from update / from the audio callback (the watchdog's cases).
#include <enaga/app.hpp>

class Crash : public enaga::App {
public:
    bool init() override { enaga::log("crash ready"); return true; }
    void on_key(int key, bool down) override {
        if (down && key == ENAGA_KEY_STEP1) main_crash_ = true;
        if (down && key == ENAGA_KEY_STEP1 + 1) audio_crash_ = true;
        if (down && key == ENAGA_KEY_STEP1 + 2) main_exception_ = true;
        if (down && key == ENAGA_KEY_STEP1 + 3) audio_exception_ = true;
        if (down && key == ENAGA_KEY_STEP1 + 4) main_hang_ = true;
        if (down && key == ENAGA_KEY_STEP1 + 5) audio_hang_ = true;
    }
    void update(uint32_t) override {
        if (main_crash_) enaga::fatal("main crash");
        if (main_exception_) asm volatile("swe");
        while (main_hang_) {}
    }
    void audio(const float *, float *out, uint32_t frames) override {
        if (audio_crash_) enaga::fatal("audio crash");
        if (audio_exception_) asm volatile("swe");
        while (audio_hang_) {}
        for (uint32_t i = 0; i < 2 * frames; i++) out[i] = 0.5f;
    }

private:
    volatile bool main_crash_ = false, audio_crash_ = false, main_exception_ = false, audio_exception_ = false;
    volatile bool main_hang_ = false, audio_hang_ = false;
};

ENAGA_APP(Crash)
