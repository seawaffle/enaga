// Synth: an eight-voice virtual analogue synthesizer, the example of a whole Enaga app.
//
// Sound: two oscillators (saw, pulse, triangle, sine) with a sub-oscillator and noise, hard sync of OSC 2 to
// OSC 1 and a ring modulator; a state-variable filter (low-pass 12 or 24 dB, band-pass, high-pass) with drive;
// three ADSR envelopes (amplitude, and ENV2 / ENV3 to assign); two LFOs that can fade in; an eight-slot modulation
// matrix; chorus, a stereo delay and a reverb. POLY, MONO or LEGATO, with glide; up to four unison voices a note.
//
// Playing: the pads are a keyboard (the lower row the white keys C to C, the upper row the black ones; or all
// fifteen chromatic), velocity from the pad and pressure as a modulation source. STEP 1-16 play the chromatic
// notes from the same C at a fixed velocity. The octave buttons shift both by an octave.
//
// Editing: F2-F8 choose a page (OSC, FILTER, ENV, LFO, MOD, FX, VOICE). A page holds one or more groups of up to
// four parameters, turned with knobs A-D; the selection buttons under the screen flip between the groups. Pushing
// a knob puts its parameter back to the patch's value. F1 is the function button: F1 + F2 opens PATCH, where
// VALUE chooses a patch, OK loads it and F1 + OK saves the sound into the chosen user slot (U1-U16, after the
// factory patches). CLR silences every note. The sound when the app stops (switching off included) is saved too,
// and is where it starts next time. Patches are files in the app's data folder (patches.hpp).
#include <enaga/app.hpp>
#include <enaga/files.hpp>

#include "engine.hpp"
#include "patches.hpp"

namespace {

using namespace synth;

struct Group {
    const char *title;
    uint8_t count;
    uint8_t params[4];            // knobs A-D
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
    {"OSC MOD", 2, {osc2_sync, mix_ring}},
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
    {"LFO 1", 4, {lfo1_shape, lfo1_rate, lfo1_sync, lfo1_fade}},
    {"LFO 2", 4, {lfo2_shape, lfo2_rate, lfo2_sync, lfo2_fade}},
};
constexpr Group kMod[] = {
#define SLOT(n) {"MOD " #n, 3, {mod_source(n - 1), mod_destination(n - 1), mod_amount(n - 1)}}
    SLOT(1), SLOT(2), SLOT(3), SLOT(4), SLOT(5), SLOT(6), SLOT(7), SLOT(8),
#undef SLOT
};
constexpr Group kFx[] = {
    {"CHORUS", 3, {chorus_rate, chorus_depth, chorus_mix}},
    {"DELAY", 4, {delay_time, delay_feedback, delay_tone, delay_mix}},
    {"REVERB", 3, {reverb_decay, reverb_damping, reverb_mix}},
    {"DELAY ROUTING", 2, {delay_mode, delay_send}},
};
constexpr Group kVoice[] = {
    {"VOICE", 4, {voice_mode, voice_glide, voice_velocity, voice_volume}},
    {"UNISON", 3, {unison_voices, unison_detune, unison_spread}},
    {"KEYS", 2, {voice_layout, voice_step_velocity}},
};
constexpr Page kPages[] = {
    {"OSC", ENAGA_KEY_F2, kOsc, 4}, {"FILTER", ENAGA_KEY_F3, kFilter, 2}, {"ENV", ENAGA_KEY_F4, kEnv, 3},
    {"LFO", ENAGA_KEY_F5, kLfo, 2}, {"MOD", ENAGA_KEY_F6, kMod, 8}, {"FX", ENAGA_KEY_F7, kFx, 4},
    {"VOICE", ENAGA_KEY_F8, kVoice, 3},
};
constexpr int kPageCount = sizeof kPages / sizeof kPages[0];
constexpr int kPatchPage = kPageCount;      // F1 + F2; page_ == kPatchPage shows the patch list
constexpr int kPatchKey = ENAGA_KEY_F2;
int entries() { return patch_count + user_slots; }   // the PATCH list: factory patches, then the user slots

// Pads by MCU channel: channels run left to right alternating lower and upper row (docs/hardware.md).
constexpr int8_t kPiano[15] = {0, 1, 2, 3, 4, -1, 5, 6, 7, 8, 9, 10, 11, -1, 12};   // semitones; -1 = no note
constexpr int kStepKey = 16;      // NoteEvent key for STEP 1; pads are 0-14
constexpr int kBase = 48;         // C3 at octave 0

class Synth : public enaga::App {
public:
    bool init() override {
        engine_.init(float(ENAGA_SAMPLE_RATE));
        scan_user();
        if (!restore_last()) load_patch(0);
        show_page_leds();
        show_pad_leds();
        enaga::log("synth ready");
        return true;
    }

    void stop() override {                    // the sound now, for next time
        uint8_t file[file_max];
        int16_t values[param_count];
        current(values);
        Saved last{{}, int16_t(patch_), 2, values, patch_values_};
        copy_name(last.name, name_);
        enaga::files::save(path(-1), file, encode(last, file));
    }

    void audio(const float *, float *out, uint32_t frames) override { engine_.render(out, frames); }

    void update(uint32_t ms) override { now_ = ms; }

    void on_card(bool usable) override {
        scan_user();
        message(usable ? "CARD IN" : "NO CARD");
    }

    void on_key(int key, bool down) override {
        if (key >= ENAGA_KEY_STEP1 && key < ENAGA_KEY_STEP1 + 16) {
            int step = key - ENAGA_KEY_STEP1;
            int note = kBase + 12 * octave_ + step;
            if (down) step_note_[step] = uint8_t(note);
            send(down ? NoteEvent::on : NoteEvent::off, kStepKey + step, down ? note : step_note_[step],
                 engine_.values[voice_step_velocity]);
            enaga::step_led(step + 1, 0, down ? 8 : 0);
            return;
        }
        if (key == ENAGA_KEY_F1) {                // the function button: held with another key
            function_ = down;
            enaga::button_led(ENAGA_KEY_F1, 0, down ? 8 : 0);
            return;
        }
        if (!down) return;
        if (function_ && key == ENAGA_KEY_OK && page_ == kPatchPage) {
            save_patch(choice_);
            return;
        }
        if (function_ && key == kPatchKey) {
            page_ = kPatchPage;
            choice_ = patch_;
            show_page_leds();
            return;
        }
        for (int p = 0; p < kPageCount; p++)
            if (key == kPages[p].key) {
                page_ = p;
                show_page_leds();
                return;
            }
        if (key <= ENAGA_KEY_PUSH_D && page_ != kPatchPage) {   // knob pushes: back to the patch's value
            int knob = key == ENAGA_KEY_PUSH_A ? 0 : key == ENAGA_KEY_PUSH_B ? 1 : key == ENAGA_KEY_PUSH_C ? 2 : 3;
            const Group &group = current_group();
            if (knob < group.count) set(group.params[knob], patch_values_[group.params[knob]]);
            return;
        }
        switch (key) {
        case ENAGA_KEY_SELECT_LEFT:
        case ENAGA_KEY_SELECT_RIGHT:
            if (page_ != kPatchPage) {
                int count = kPages[page_].count;
                group_[page_] = (group_[page_] + (key == ENAGA_KEY_SELECT_RIGHT ? 1 : count - 1)) % count;
            }
            break;
        case ENAGA_KEY_OK: if (page_ == kPatchPage) load_patch(choice_); break;
        case ENAGA_KEY_OCTAVE_LEFT: if (octave_ > -3) octave_--; break;
        case ENAGA_KEY_OCTAVE_RIGHT: if (octave_ < 3) octave_++; break;
        case ENAGA_KEY_CLR: send(NoteEvent::all_off, 0, 0, 0); break;
        default: break;
        }
    }

    void on_encoder(int encoder, int delta) override {
        if (page_ == kPatchPage) {
            if (encoder == 4) choice_ = (choice_ + delta % entries() + entries()) % entries();
            return;
        }
        const Group &group = current_group();
        if (encoder >= group.count) return;       // VALUE (4) and knobs past the group's end do nothing here
        set(group.params[encoder], engine_.values[group.params[encoder]] + delta);
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
        entry_name(patch_, text);
        canvas.text(0, 0, text, 15);
        text_number(text, "V", int(engine_.active_voices));
        canvas.text(80, 0, text, 8);
        text_number(text, "", int(enaga::audio_load() / 10));
        append(text, "%");
        canvas.text(128 - 6 * length(text), 0, text, 8);

        canvas.rect(0, 10, 128, 1, 4);
        if (page_ == kPatchPage) draw_patches(canvas);
        else draw_group(canvas);
        if (octave_) {
            text_number(text, octave_ > 0 ? "OCT+" : "OCT", octave_);
            canvas.text(0, 92, text, 8);
        }
        if (int32_t(message_until_ - now_) > 0) canvas.text(128 - 6 * length(message_), 92, message_, 15);
        draw_scope(canvas, 98, 30);
    }

private:
    const Group &current_group() const { return kPages[page_].groups[group_[page_]]; }

    void set(int param, int value) {
        const ParamInfo &p = info(param);
        engine_.values[param] = int16_t(value < p.min ? p.min : value > p.max ? p.max : value);
        if (param == voice_layout) show_pad_leds();
        if (param == voice_mode) send(NoteEvent::all_off, 0, 0, 0);
    }

    void draw_group(enaga::Canvas &canvas) {
        char text[24];
        const Page &page = kPages[page_];
        const Group &group = current_group();
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
            char label[2] = {char('A' + row), 0};
            canvas.text(0, y, label, 6);
            canvas.text(9, y, p.name, 12);
            format(param, value, text);
            canvas.text(128 - 6 * length(text), y, text, value == patch_values_[param] ? 15 : 11);
            int width = 40 * (value - p.min) / (p.max - p.min);   // a bar under the name
            canvas.rect(9, y + 8, 40, 1, 2);
            canvas.rect(9, y + 8, width, 1, 9);
        }
    }

    // The patch list: the chosen entry highlighted, the loaded one marked with a block, empty user slots dim.
    void draw_patches(enaga::Canvas &canvas) {
        char text[24];
        canvas.text(0, 14, "PATCH", 15);
        canvas.text(128 - 6 * 10, 14, "F1+OK SAVE", 6);
        int first = choice_ - 3;
        first = first < 0 ? 0 : first > entries() - 6 ? entries() - 6 : first;
        for (int row = 0; row < 6; row++) {
            int n = first + row, y = 26 + 11 * row;
            bool empty = n >= patch_count && !user_used_[n - patch_count];
            if (n == choice_) canvas.rect(0, y - 2, 128, 11, 3);
            if (n == patch_) canvas.rect(1, y + 1, 4, 5, 15);   // the loaded one
            entry_name(n, text);
            canvas.text(9, y, text, empty ? 5 : n == choice_ ? 15 : 10);
        }
    }

    // "BRASS", "U3 BRASS" (a user slot and the name saved in it) or "U4 -" (an empty slot).
    void entry_name(int n, char *text) const {
        if (n < patch_count) {
            copy_name(text, patches[n].name);
            return;
        }
        int slot = n - patch_count;
        text_number(text, "U", slot + 1);
        append(text, " ");
        append(text, n == patch_ ? name_ : user_used_[slot] ? user_name_[slot] : "-");
    }

    // The card's files: "<data folder>/USER 01.SYN" for slot 0, LAST.SYN for -1.
    static const char *path(int slot) {
        static char text[64];
        text[0] = 0;
        append(text, enaga::files::data_folder());
        if (slot < 0) {
            append(text, "/LAST.SYN");
        } else {
            append(text, slot < 9 ? "/USER 0" : "/USER ");
            text_number(text + length(text), "", slot + 1);
            append(text, ".SYN");
        }
        return text;
    }

    void scan_user() {                        // which user slots hold a patch, and their names
        for (int slot = 0; slot < user_slots; slot++) {
            uint8_t file[file_max];
            int16_t entry, values[param_count];
            int32_t got = enaga::files::read_all(path(slot), file, sizeof file);
            user_used_[slot] = decode(file, got, user_name_[slot], &entry, values, nullptr);
        }
    }

    bool restore_last() {
        uint8_t file[file_max];
        int16_t entry, values[param_count];
        int32_t got = enaga::files::read_all(path(-1), file, sizeof file);
        if (!decode(file, got, name_, &entry, values, patch_values_)) return false;
        patch_ = entry >= 0 && entry < entries() ? entry : 0;
        choice_ = patch_;
        apply(values);
        return true;
    }

    void save_patch(int n) {
        if (n < patch_count) {
            message("USER SLOTS ONLY");
            return;
        }
        int slot = n - patch_count;
        uint8_t file[file_max];
        int16_t values[param_count];
        current(values);
        Saved saved{{}, -1, 1, values, nullptr};
        copy_name(saved.name, name_);
        int32_t result = enaga::files::save(path(slot), file, encode(saved, file));
        if (result < 0) {
            message(enaga::files::error_name(result));
            return;
        }
        user_used_[slot] = true;
        copy_name(user_name_[slot], name_);
        patch_ = n;
        for (int p = 0; p < param_count; p++) patch_values_[p] = values[p];
        char text[24];
        text_number(text, "SAVED U", slot + 1);
        message(text);
    }

    void message(const char *text) {          // shown for two seconds
        copy_name(message_, text, sizeof message_);
        message_until_ = now_ + 2000;
    }

    void send(NoteEvent::Type type, int key, int note, int value) {
        engine_.post({type, uint8_t(key), uint8_t(note), uint8_t(value)});
    }

    void load_patch(int n) {
        if (n < patch_count) {
            synth::load(patches[n], patch_values_);
            copy_name(name_, patches[n].name);
        } else {
            uint8_t file[file_max];
            int16_t entry;
            int32_t got = enaga::files::read_all(path(n - patch_count), file, sizeof file);
            if (!decode(file, got, name_, &entry, patch_values_, nullptr)) {
                message(got < 0 && got != ENAGA_FILE_NOT_FOUND ? enaga::files::error_name(got) : "EMPTY SLOT");
                return;
            }
        }
        patch_ = n;
        apply(patch_values_);
    }

    void current(int16_t *values) const {
        for (int p = 0; p < param_count; p++) values[p] = engine_.values[p];
    }

    void apply(const int16_t *values) {
        send(NoteEvent::all_off, 0, 0, 0);    // the voice mode may change
        for (int p = 0; p < param_count; p++) engine_.values[p] = values[p];
        show_pad_leds();
    }

    void show_page_leds() {                   // the page's F key green; PATCH lights F2 red
        for (int p = 0; p < kPageCount; p++)
            enaga::button_led(kPages[p].key, kPages[p].key == kPatchKey && page_ == kPatchPage ? 8 : 0, p == page_ ? 8 : 0);
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
    static void copy_name(char *out, const char *in, int size = name_size) {
        int i = 0;
        while (i < size - 1 && in[i]) { out[i] = in[i]; i++; }
        out[i] = 0;
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
    int page_ = 0, patch_ = 0, choice_ = 0, octave_ = 0;   // patch_, choice_: PATCH list entries
    char name_[name_size] = {};               // the loaded patch's name
    bool user_used_[user_slots] = {};
    char user_name_[user_slots][name_size] = {};
    char message_[22] = {};
    uint32_t now_ = 0, message_until_ = 0;
    bool function_ = false;                   // F1 held
    int16_t patch_values_[param_count] = {};  // the loaded patch, for knob-push resets
    int group_[kPageCount] = {};
    uint8_t pad_note_[15] = {}, step_note_[16] = {};
    bool pad_held_[15] = {};
    int pad_pressure_[15] = {};
};

}  // namespace

ENAGA_APP(Synth)
