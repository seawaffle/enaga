// Widgets: an example of the UI toolkit (<enaga/ui.hpp>). Five pages, chosen with F2-F6:
//
//   F2  SOUNDS   a List: UP / DOWN or VALUE move, OK picks the sound (its name goes in the status line)
//   F3  EDIT     a ParamPage: knobs A-D turn the parameters, the selection buttons flip between the two groups,
//                pushing a knob returns it to its initial value
//   F4  NAME     a TextEntry for the name shown in the status line: OK keeps it, CLR leaves it as it was
//   F5  RESET    a Dialog asking whether to reset the parameters: OK resets, CLR leaves them
//   F6  LEVELS   meters for the input, with peak marks, and a centred bar on knob A (PAN)
//
// Every page has the status line at the top and key hints at the bottom, where a Notice says what just happened
// for a moment ("BRASS CHOSEN"). Text is built with Line. What the widgets report is also logged ("CHOSE 2",
// "CHANGED 4 70", "NAME DRONE", "RESET YES"), which is how tests/test_widgets.py follows it on the emulator. The
// app makes no sound: it is about the screen.
#include <enaga/app.hpp>
#include <enaga/ui.hpp>

namespace {

using enaga::ui::Action;
using enaga::ui::Line;

const char *const kSounds[] = {"PIANO", "STRINGS", "BRASS", "ORGAN", "BELL", "PAD", "BASS", "LEAD", "PLUCK",
                               "CHOIR", "DRONE", "NOISE"};
constexpr int kSoundCount = sizeof kSounds / sizeof kSounds[0];

const char *const kWaves[] = {"SAW", "PULSE", "TRI", "SINE"};
void hertz(int value, char *out) {           // CUTOFF 0-127 shown as a rough frequency
    Line().number(20 + value * value).add("HZ").copy_to(out, 13);
}

const enaga::ui::Param kParams[] = {
    {"WAVE", 0, 3, 0, kWaves},
    {"CUTOFF", 0, 127, 64, nullptr, hertz},
    {"RESONANCE", 0, 100, 20, nullptr, nullptr, "%"},
    {"ATTACK", 0, 2000, 5, nullptr, nullptr, "MS"},
    {"RELEASE", 0, 4000, 300, nullptr, nullptr, "MS"},
    {"LEVEL", 0, 100, 80, nullptr, nullptr, "%"},
};
constexpr int kParamCount = sizeof kParams / sizeof kParams[0];

// The input's peak in each block, written by audio() and read by update(): one writer, one float each.
volatile float g_block_peak[2];

}  // namespace

class Widgets : public enaga::App {
public:
    bool init() override {
        sounds_.set_title("SOUNDS");
        sounds_.set_items(kSounds, kSoundCount);
        for (int i = 0; i < kParamCount; i++) values_[i] = kParams[i].initial;
        edit_.set("EDIT", kParams, kParamCount, values_);
        name_.add("MY SOUND");
        return true;
    }

    void on_key(int key, bool down) override {
        if (dialog_.active()) {                   // an open dialog or text entry takes every key
            Action a = dialog_.key(key, down);
            if (a == Action::yes) {
                for (int i = 0; i < kParamCount; i++) values_[i] = kParams[i].initial;
                say("RESET YES", "PARAMETERS RESET");
            } else if (a == Action::no) {
                say("RESET NO", "NOTHING RESET");
            }
            if (!dialog_.active()) page_ = kEdit;
            return;
        }
        if (entry_.active()) {
            Action a = entry_.key(key, down);
            if (a == Action::chosen) {
                name_.clear().add(entry_.text());
                say(Line("NAME ").add(name_), "NAME CHANGED");
            }
            if (a == Action::no) say("NAME KEPT", "NAME KEPT");
            if (!entry_.active()) page_ = kSounds_;
            return;
        }
        if (down && key == ENAGA_KEY_F2) { page_ = kSounds_; return; }
        if (down && key == ENAGA_KEY_F3) { page_ = kEdit; return; }
        if (down && key == ENAGA_KEY_F4) { entry_.open("NAME", name_, 12); page_ = kName; return; }
        if (down && key == ENAGA_KEY_F5) { dialog_.open("RESET ALL\nPARAMETERS?"); page_ = kReset; return; }
        if (down && key == ENAGA_KEY_F6) { page_ = kLevels; return; }
        if (page_ == kSounds_) {
            if (sounds_.key(key, down) == Action::chosen) {
                chosen_ = sounds_.cursor();
                say(Line("CHOSE ").number(chosen_), Line(kSounds[chosen_]).add(" CHOSEN"));
            }
        } else if (page_ == kEdit) {
            if (edit_.key(key, down) == Action::changed) log_change();
        } else if (page_ == kLevels && down && key == ENAGA_KEY_PUSH_A) {
            pan_ = 0;
        }
    }

    void on_encoder(int encoder, int delta) override {
        if (entry_.active()) { entry_.encoder(encoder, delta); return; }
        if (dialog_.active()) return;
        if (page_ == kSounds_) sounds_.encoder(encoder, delta);
        else if (page_ == kEdit && edit_.encoder(encoder, delta) == Action::changed) log_change();
        else if (page_ == kLevels && encoder == 0) {
            pan_ += delta;
            pan_ = pan_ < -50 ? -50 : pan_ > 50 ? 50 : pan_;
        }
    }

    void audio(const float *in, float *out, uint32_t frames) override {
        float peak[2] = {0.0f, 0.0f};
        for (uint32_t i = 0; i < frames; i++) {
            for (int c = 0; c < 2; c++) {
                float v = in[2 * i + c] < 0.0f ? -in[2 * i + c] : in[2 * i + c];
                peak[c] = v > peak[c] ? v : peak[c];
                out[2 * i + c] = 0.0f;
            }
        }
        g_block_peak[0] = peak[0];
        g_block_peak[1] = peak[1];
    }

    // The meters: the level falls about 20 dB a second, and the peak mark holds for a second before it falls.
    void update(uint32_t ms) override {
        for (int c = 0; c < 2; c++) {
            float now = enaga::ui::meter_db(g_block_peak[c]);
            level_[c] = now > level_[c] ? now : level_[c] - 0.008f;
            if (now >= peak_[c]) { peak_[c] = now; peak_ms_[c] = ms; }
            else if (ms - peak_ms_[c] > 1000) peak_[c] -= 0.008f;
            if (level_[c] < 0.0f) level_[c] = 0.0f;
            if (peak_[c] < 0.0f) peak_[c] = 0.0f;
        }
    }

    void draw(enaga::Canvas &canvas) override {
        using namespace enaga::ui;
        canvas.clear();
        status_line(canvas, name_, kSounds[chosen_]);
        if (page_ == kSounds_) {
            sounds_.draw(canvas, body_top, body_bottom - 10);
            hint_line(canvas, "OK: CHOOSE", 1);
        } else if (page_ == kEdit || page_ == kReset) {
            edit_.draw(canvas, body_top);
            hint_line(canvas, "PUSH: INITIAL VALUE", 1);
        } else if (page_ == kName) {
            entry_.draw(canvas, body_top);
        } else {
            draw_levels(canvas);
        }
        dialog_.draw(canvas);                     // over the page, when open
        hint_line(canvas, "PAGES: F2 F3 F4 F5 F6");
        notice_.draw(canvas);                     // over the hints, for a moment
    }

private:
    enum Page { kSounds_, kEdit, kName, kReset, kLevels };

    void draw_levels(enaga::Canvas &canvas) {
        using namespace enaga::ui;
        canvas.text(0, body_top, "LEVELS", 15);
        const char *names[2] = {"IN L", "IN R"};
        for (int c = 0; c < 2; c++) {
            int x = 8 + 40 * c;
            meter(canvas, x, body_top + 14, 8, 50, level_[c], peak_[c]);
            canvas.text(x - 4, body_top + 68, names[c], 10);
        }
        Line pan("PAN ");
        if (pan_ == 0) pan.add("C");
        else pan.add(pan_ < 0 ? "L" : "R").number(pan_ < 0 ? -pan_ : pan_);
        text_right(canvas, body_top + 28, pan, 15);
        bar_centred(canvas, 88, body_top + 40, 40, pan_, -50, 50, 2);
        hint_line(canvas, "A: PAN  PUSH A: CENTRE", 1);
    }

    // Logs `log` for the tests and shows `notice` on the screen.
    void say(const char *log, const char *notice) {
        enaga::log(log);
        notice_.show(notice);
    }

    void log_change() {
        int p = edit_.changed();
        enaga::log(Line("CHANGED ").number(p).add(' ').number(values_[p]));
    }

    enaga::ui::List sounds_;
    enaga::ui::ParamPage edit_;
    enaga::ui::TextEntry entry_;
    enaga::ui::Dialog dialog_;
    enaga::ui::Notice notice_;
    int values_[kParamCount] = {};
    Line name_;
    int chosen_ = 0, pan_ = 0;
    float level_[2] = {}, peak_[2] = {};
    uint32_t peak_ms_[2] = {};
    Page page_ = kSounds_;
};

ENAGA_APP(Widgets)
