// Synth: an eight-voice virtual analogue synthesizer, the example of a whole Enaga app.
//
// Sound: two oscillators (saw, pulse, triangle, sine) with a sub-oscillator and noise; a state-variable filter
// (low-pass 12 or 24 dB, band-pass, high-pass) with drive; three ADSR envelopes (amplitude, and ENV2 / ENV3 to
// assign); two LFOs; an eight-slot modulation matrix; a stereo delay and a reverb. POLY, MONO or LEGATO, with glide.
//
// Playing: the pads are a keyboard (the lower row the white keys C to C, the upper row the black ones; or all
// fifteen chromatic), velocity from the pad and pressure as a modulation source. STEP 1-16 play the chromatic
// notes from the same C at a fixed velocity. The octave buttons shift both by an octave.
//
// Editing: F2-F8 choose the page (OSC, FILTER, ENV, LFO, MOD, FX, VOICE). Each page has groups of up to five
// parameters; left / right change the group. Knobs A-D change the group's first four; up / down move the cursor
// and VALUE changes the parameter under it. The selection buttons step through the factory patches; CLR
// silences every note. Edits last until the app stops (saving patches needs Enaga's file API, E2).
#include <enaga/app.hpp>

#include "engine.hpp"
#include "patches.hpp"

namespace {

using namespace synth;

struct Group {
    const char *title;
    uint8_t count;
    uint8_t params[5];
};
struct Page {
    const char *name;
    int key;
    const Group *groups;
    int count;
};

constexpr Group kOsc[] = {
    {"OSC 1", 4, {osc1_wave, osc1_width, osc1_semi, osc1_fine}},
    {"OSC 2", 4, {osc2_wave, osc2_width, osc2_semi, osc2_fine}},
    {"MIXER", 4, {mix_osc1, mix_osc2, mix_sub, mix_noise}},
};
constexpr Group kFilter[] = {
    {"FILTER", 4, {filter_type, filter_cutoff, filter_resonance, filter_drive}},
    {"FILTER MOD", 3, {filter_env, filter_key, filter_velocity}},
};
constexpr Group kEnv[] = {
    {"AMP ENV", 4, {amp_attack, amp_decay, amp_sustain, amp_release}},
    {"ENV 2", 4, {env2_attack, env2_decay, env2_sustain, env2_release}},
    {"ENV 3", 4, {env3_attack, env3_decay, env3_sustain, env3_release}},
};
constexpr Group kLfo[] = {
    {"LFO 1", 3, {lfo1_shape, lfo1_rate, lfo1_sync}},
    {"LFO 2", 3, {lfo2_shape, lfo2_rate, lfo2_sync}},
};
constexpr Group kMod[] = {
#define SLOT(n) {"MOD " #n, 3, {mod_source(n - 1), mod_destination(n - 1), mod_amount(n - 1)}}
    SLOT(1), SLOT(2), SLOT(3), SLOT(4), SLOT(5), SLOT(6), SLOT(7), SLOT(8),
#undef SLOT
};
constexpr Group kFx[] = {
    {"DELAY", 5, {delay_time, delay_feedback, delay_tone, delay_mix, delay_mode}},
    {"REVERB", 3, {reverb_decay, reverb_damping, reverb_mix}},
};
constexpr Group kVoice[] = {
    {"VOICE", 5, {voice_mode, voice_glide, voice_velocity, voice_volume, voice_layout}},
};
constexpr Page kPages[] = {
    {"OSC", ENAGA_KEY_F2, kOsc, 3}, {"FILTER", ENAGA_KEY_F3, kFilter, 2}, {"ENV", ENAGA_KEY_F4, kEnv, 3},
    {"LFO", ENAGA_KEY_F5, kLfo, 2}, {"MOD", ENAGA_KEY_F6, kMod, 8}, {"FX", ENAGA_KEY_F7, kFx, 2},
    {"VOICE", ENAGA_KEY_F8, kVoice, 1},
};
constexpr int kPageCount = sizeof kPages / sizeof kPages[0];

// Pads by MCU channel: channels run left to right alternating lower and upper row (docs/hardware.md).
constexpr int8_t kPiano[15] = {0, 1, 2, 3, 4, -1, 5, 6, 7, 8, 9, 10, 11, -1, 12};   // semitones; -1 = no note
constexpr int kStepKey = 16;      // NoteEvent key for STEP 1; pads are 0-14
constexpr int kBase = 48;         // C3 at octave 0

class Synth : public enaga::App {
public:
    bool init() override {
        engine_.init(float(ENAGA_SAMPLE_RATE));
        load_patch(0);
        show_page_leds();
        show_pad_leds();
        enaga::log("synth ready");
        return true;
    }

    void audio(const float *, float *out, uint32_t frames) override { engine_.render(out, frames); }

    void on_key(int key, bool down) override {
        if (key >= ENAGA_KEY_STEP1 && key < ENAGA_KEY_STEP1 + 16) {
            int step = key - ENAGA_KEY_STEP1;
            int note = kBase + 12 * octave_ + step;
            if (down) step_note_[step] = uint8_t(note);
            send(down ? NoteEvent::on : NoteEvent::off, kStepKey + step, down ? note : step_note_[step], 100);
            enaga::step_led(step + 1, 0, down ? 8 : 0);
            return;
        }
        if (!down) return;
        for (int p = 0; p < kPageCount; p++)
            if (key == kPages[p].key) {
                page_ = p;
                cursor_ = 0;
                show_page_leds();
                return;
            }
        const Page &page = kPages[page_];
        switch (key) {
        case ENAGA_KEY_LEFT: group_[page_] = (group_[page_] + page.count - 1) % page.count; cursor_ = 0; break;
        case ENAGA_KEY_RIGHT: group_[page_] = (group_[page_] + 1) % page.count; cursor_ = 0; break;
        case ENAGA_KEY_UP: if (cursor_ > 0) cursor_--; break;
        case ENAGA_KEY_DOWN: if (cursor_ + 1 < page.groups[group_[page_]].count) cursor_++; break;
        case ENAGA_KEY_SELECT_LEFT: load_patch((patch_ + patch_count - 1) % patch_count); break;
        case ENAGA_KEY_SELECT_RIGHT: load_patch((patch_ + 1) % patch_count); break;
        case ENAGA_KEY_OCTAVE_LEFT: if (octave_ > -3) octave_--; break;
        case ENAGA_KEY_OCTAVE_RIGHT: if (octave_ < 3) octave_++; break;
        case ENAGA_KEY_CLR: send(NoteEvent::all_off, 0, 0, 0); break;
        default: break;
        }
    }

    void on_encoder(int encoder, int delta) override {
        const Group &group = kPages[page_].groups[group_[page_]];
        int row = encoder == 4 ? cursor_ : encoder;
        if (row >= group.count) return;
        if (encoder != 4) cursor_ = row;
        int param = group.params[row];
        const ParamInfo &p = info(param);
        int value = engine_.values[param] + delta;
        engine_.values[param] = int16_t(value < p.min ? p.min : value > p.max ? p.max : value);
        if (param == voice_layout) show_pad_leds();
        if (param == voice_mode) send(NoteEvent::all_off, 0, 0, 0);
    }

    void on_pad_press(int pad, int velocity) override {
        int semitone = engine_.values[voice_layout] == layout_piano ? kPiano[pad] : pad;
        if (semitone < 0) return;
        pad_note_[pad] = uint8_t(kBase + 12 * octave_ + semitone);
        pad_held_[pad] = true;
        pad_pressure_[pad] = 0;
        send(NoteEvent::on, pad, pad_note_[pad], velocity);
        enaga::pad_led(pad, 0, 2 + unsigned(velocity) * 8 / 127, 0);
    }

    void on_pad_release(int pad) override {
        if (!pad_held_[pad]) return;
        pad_held_[pad] = false;
        send(NoteEvent::off, pad, pad_note_[pad], 0);
        show_pad_led(pad);
    }

    void on_pad(int pad, unsigned reading) override {
        if (!pad_held_[pad]) return;
        // Pressure over the press threshold: 0 at 800 to 127 at 12,000 (provisional, like Enaga's thresholds).
        int pressure = reading <= 800 ? 0 : reading >= 12000 ? 127 : int((reading - 800) * 127 / 11200);
        if (pressure - pad_pressure_[pad] >= 2 || pad_pressure_[pad] - pressure >= 2) {
            pad_pressure_[pad] = pressure;
            send(NoteEvent::pressure, pad, pad_note_[pad], pressure);
        }
    }

    void draw(enaga::Canvas &canvas) override {
        char text[24];
        canvas.clear();
        canvas.text(0, 0, patches[patch_].name, 15);
        text_number(text, "V", int(engine_.active_voices));
        canvas.text(80, 0, text, 8);
        text_number(text, "", int(enaga::audio_load() / 10));
        append(text, "%");
        canvas.text(128 - 6 * length(text), 0, text, 8);

        const Page &page = kPages[page_];
        const Group &group = page.groups[group_[page_]];
        canvas.rect(0, 10, 128, 1, 4);
        canvas.text(0, 14, group.title, 15);
        text[0] = 0;                          // the page, and the group's place in it: "OSC 1/3"
        append(text, page.name);
        if (page.count > 1) {
            text_number(text + length(text), " ", group_[page_] + 1);
            append(text, "/");
            text_number(text + length(text), "", page.count);
        }
        if (page.count > 1 || !same(page.name, group.title)) canvas.text(128 - 6 * length(text), 14, text, 6);

        for (int row = 0; row < group.count; row++) {
            int y = 26 + 13 * row, param = group.params[row];
            const ParamInfo &p = info(param);
            int value = engine_.values[param];
            if (row == cursor_) canvas.rect(0, y - 2, 128, 12, 3);
            char label[2] = {row < 4 ? char('A' + row) : ' ', 0};
            canvas.text(0, y, label, 6);
            canvas.text(9, y, p.name, 12);
            format(param, value, text);
            canvas.text(128 - 6 * length(text), y, text, 15);
            int width = 40 * (value - p.min) / (p.max - p.min);   // a bar under the name
            canvas.rect(9, y + 8, 40, 1, 2);
            canvas.rect(9, y + 8, width, 1, 9);
        }
        if (octave_) {
            text_number(text, octave_ > 0 ? "OCT+" : "OCT", octave_);
            canvas.text(0, 92, text, 8);
        }
        draw_scope(canvas, 98, 30);
    }

private:
    void send(NoteEvent::Type type, int key, int note, int value) {
        engine_.post({type, uint8_t(key), uint8_t(note), uint8_t(value)});
    }

    void load_patch(int n) {
        patch_ = n;
        synth::load(patches[n], engine_.values);
        show_pad_leds();
    }

    void show_page_leds() {
        for (int p = 0; p < kPageCount; p++) enaga::button_led(kPages[p].key, 0, p == page_ ? 8 : 0);
    }
    void show_pad_led(int pad) {
        bool note = engine_.values[voice_layout] != layout_piano || kPiano[pad] >= 0;
        enaga::pad_led(pad, 0, 0, note ? 1 : 0);
    }
    void show_pad_leds() {
        for (int pad = 0; pad < 15; pad++)
            if (!pad_held_[pad]) show_pad_led(pad);
    }

    // The output, from its first rising zero crossing, so a steady note stands still.
    void draw_scope(enaga::Canvas &canvas, int top, int height) {
        const uint32_t end = engine_.scope_write;
        const uint32_t mask = Engine::kScope - 1;
        uint32_t start = end - Engine::kScope + 1;
        for (uint32_t i = 0; i < Engine::kScope - 129; i++)
            if (engine_.scope[(start + i) & mask] < 0.0f && engine_.scope[(start + i + 1) & mask] >= 0.0f) {
                start += i + 1;
                break;
            }
        canvas.rect(0, top + height / 2, 128, 1, 2);
        int last = -1;
        for (int x = 0; x < 128; x++) {
            float s = 0.5f * engine_.scope[(start + uint32_t(x)) & mask];   // left + right
            int y = top + height / 2 - int(s * float(height / 2));
            y = y < top ? top : y >= top + height ? top + height - 1 : y;
            if (last >= 0)
                for (int j = (last < y ? last : y); j <= (last < y ? y : last); j++) canvas.pixel(x, j, 12);
            last = y;
        }
    }

    static int length(const char *s) {
        int n = 0;
        while (s[n]) n++;
        return n;
    }
    static bool same(const char *a, const char *b) {
        while (*a && *a == *b) a++, b++;
        return *a == *b;
    }
    static void append(char *text, const char *more) {
        text += length(text);
        while ((*text++ = *more++)) {}
    }
    static void text_number(char *text, const char *prefix, int value) {
        while ((*text = *prefix++)) text++;
        if (value < 0) { *text++ = '-'; value = -value; }
        char digits[12];
        int n = 0;
        do { digits[n++] = char('0' + value % 10); value /= 10; } while (value);
        while (n) *text++ = digits[--n];
        *text = 0;
    }

    Engine engine_;
    int page_ = 0, cursor_ = 0, patch_ = 0, octave_ = 0;
    int group_[kPageCount] = {};
    uint8_t pad_note_[15] = {}, step_note_[16] = {};
    bool pad_held_[15] = {};
    int pad_pressure_[15] = {};
};

}  // namespace

ENAGA_APP(Synth)
