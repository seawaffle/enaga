// Enaga: the C++ app interface over <enaga/abi.h>. Provisional until 1.0.
//
//     #include <enaga/app.hpp>
//     class Synth : public enaga::App {
//         bool init() override { enaga::log("hello"); return true; }
//     };
//     ENAGA_APP(Synth)
//
// The app object is constructed after the app's static constructors have run and destroyed never: Enaga
// discards an app's memory when unloading it, after calling stop(). Compiled -fno-exceptions -fno-rtti.
#pragma once

#include <enaga/abi.h>

namespace enaga {

const enaga_host_t &host();                 // defined by the app runtime

inline void log(const char *text) { host().log(text); }
inline uint32_t ms() { return host().ms(); }
inline uint32_t audio_load() { return host().audio_load(); }          // per mille of the block budget
inline uint32_t audio_load_peak() { return host().audio_load_peak(); }

// Panel LEDs, levels 0 (off) to ENAGA_LED_MAX (enaga_led in abi.h). Main context only: ignored from audio().
inline void pad_led(int pad, unsigned red, unsigned green, unsigned blue) {
    host().led(ENAGA_LED_PAD + pad, red, green, blue);       // pad 0-14, as in on_pad
}
inline void step_led(int step, unsigned red, unsigned green) {   // STEP 1-16
    host().led(int(ENAGA_LED_KEY) + ENAGA_KEY_STEP1 + step - 1, red, green, 0);
}
inline void button_led(int key, unsigned red, unsigned green) {  // enaga_key of a button with an LED
    host().led(int(ENAGA_LED_KEY) + key, red, green, 0);
}
inline void vol_led(int channel, unsigned level) {               // 0 = VOL L, 1 = VOL R
    host().led(ENAGA_LED_VOL_L + channel, level, level, level);
}
inline void mic_led(unsigned level) { host().led(ENAGA_LED_MIC, level, level, level); }
inline void value_led(unsigned red, unsigned green, unsigned blue) { host().led(ENAGA_LED_VALUE, red, green, blue); }

namespace system {
// Opens the Enaga menu once the current callback returns, as F1 + POWER does; the app is paused meanwhile.
inline void open_menu() { host().open_menu(); }
// While busy, Enaga does not switch the unit off for being idle (the AUTO OFF setting). Cleared when the app stops.
inline void set_busy(bool busy) { host().set_busy(busy ? 1u : 0u); }
// Mutes (false) or releases (true) the built-in speaker, e.g. while monitoring the built-in microphone. The user's
// SPEAKER setting still wins, and headphones cut the speaker anyway. Released again when the app stops.
inline void speaker(bool on) { host().speaker(on ? 1u : 0u); }
// The jack detect input, 0 or 1. Provisional: which jack (headphones or INPUT) and its polarity are unconfirmed.
inline int jack() { return int(host().jack()); }
}  // namespace system

// The draw target: ENAGA_SCREEN_W x ENAGA_SCREEN_H bytes, row-major, grey 0-15.
class Canvas {
public:
    explicit Canvas(uint8_t *pixels) : pixels_(pixels) {}
    static constexpr int width = ENAGA_SCREEN_W, height = ENAGA_SCREEN_H;
    void clear(uint8_t grey = 0) { for (int i = 0; i < width * height; i++) pixels_[i] = grey; }
    void pixel(int x, int y, uint8_t grey) {
        if (unsigned(x) < unsigned(width) && unsigned(y) < unsigned(height)) pixels_[y * width + x] = grey;
    }
    void rect(int x, int y, int w, int h, uint8_t grey) {
        for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) pixel(i, j, grey);
    }
    void text(int x, int y, const char *s, uint8_t grey = 15) { host().text(pixels_, x, y, s, grey); }
    uint8_t *data() { return pixels_; }

private:
    uint8_t *pixels_;
};
[[noreturn]] void fatal(const char *reason);

class App {
public:
    virtual ~App() = default;
    virtual bool init() { return true; }
    virtual void update(uint32_t ms) { (void)ms; }
    virtual void draw(Canvas &canvas) { (void)canvas; }
    virtual void stop() {}
    virtual void on_key(int key, bool down) { (void)key; (void)down; }   // key: enaga_key or bit number
    virtual void on_encoder(int encoder, int delta) { (void)encoder; (void)delta; }
    virtual void on_pad(int pad, unsigned pressure) { (void)pad; (void)pressure; }   // every raw reading, 0-16383
    virtual void on_pad_press(int pad, int velocity) { (void)pad; (void)velocity; }   // velocity 1-127
    virtual void on_pad_release(int pad) { (void)pad; }
    // The card: true when one was inserted and mounted, false when it was removed (or an inserted one cannot be
    // used). Open files from before a removal are dead: close them (<enaga/files.hpp>).
    virtual void on_card(bool usable) { (void)usable; }
    // Audio context: see enaga_app_t::audio. Interleaved stereo, `frames` frames.
    virtual void audio(const float *in, float *out, uint32_t frames) {
        (void)in;
        for (uint32_t i = 0; i < 2 * frames; i++) out[i] = 0.0f;
    }
};

namespace detail {
inline int init(void *app) { return static_cast<App *>(app)->init() ? 0 : -1; }
inline void update(void *app, uint32_t ms) { static_cast<App *>(app)->update(ms); }
inline void draw(void *app, uint8_t *pixels) {
    Canvas canvas(pixels);
    static_cast<App *>(app)->draw(canvas);
}
inline void stop(void *app) { static_cast<App *>(app)->stop(); }
inline void event(void *app, const enaga_event_t *e) {
    App *a = static_cast<App *>(app);
    if (e->type == ENAGA_EVENT_KEY) a->on_key(e->id, e->value != 0);
    else if (e->type == ENAGA_EVENT_ENCODER) a->on_encoder(e->id, e->value);
    else if (e->type == ENAGA_EVENT_PAD) a->on_pad(e->id, unsigned(e->value));
    else if (e->type == ENAGA_EVENT_PAD_PRESS) a->on_pad_press(e->id, e->value);
    else if (e->type == ENAGA_EVENT_PAD_RELEASE) a->on_pad_release(e->id);
    else if (e->type == ENAGA_EVENT_CARD) a->on_card(e->value > 0);
}
inline void audio(void *app, const float *in, float *out, uint32_t frames) {
    static_cast<App *>(app)->audio(in, out, frames);
}

template <class T> const enaga_app_t *bind(const char *name) {
    static T instance;
    static const enaga_app_t table = {ENAGA_ABI_VERSION, sizeof(enaga_app_t), name, static_cast<App *>(&instance),
                                      &detail::init, &detail::update, &detail::draw, &detail::stop,
                                      &detail::event, &detail::audio};
    return &table;
}
}  // namespace detail

}  // namespace enaga

// Defines the app: Type derives from enaga::App and is default-constructible. Enaga's menu shows the
// package name; the table's name (the type name) is what the app reports at run time.
#define ENAGA_APP(Type) \
    extern "C" const enaga_app_t *enaga_app_main(void) { return ::enaga::detail::bind<Type>(#Type); }
