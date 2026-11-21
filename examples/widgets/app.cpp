// Widgets: an example of the UI toolkit (<enaga/ui.hpp>). Four pages, chosen with F2-F5:
//
//   F2  SOUNDS   a List: UP / DOWN or VALUE move, OK picks the sound (its name goes in the header)
//   F3  EDIT     a ParamPage: knobs A-D turn the parameters, the selection buttons flip between the two groups,
//                pushing a knob returns it to its initial value
//   F4  NAME     a TextEntry for the name shown in the header: OK keeps it, CLR leaves it as it was
//   F5  RESET    a Dialog asking whether to reset the parameters: OK resets, CLR leaves them
//
// Each page sends its keys and knobs to its widget and draws it below a one-line header. What the widgets report
// is also logged ("CHOSE 2", "CHANGED 4 70", "NAME DRONE", "RESET YES"), which is how tests/test_widgets.py follows
// it on the emulator. The app makes no sound: it is about the screen.
#include <enaga/app.hpp>
#include <enaga/ui.hpp>

namespace {

using enaga::ui::Action;

const char *const kSounds[] = {"PIANO", "STRINGS", "BRASS", "ORGAN", "BELL", "PAD", "BASS", "LEAD", "PLUCK",
                               "CHOIR", "DRONE", "NOISE"};
constexpr int kSoundCount = sizeof kSounds / sizeof kSounds[0];

const char *const kWaves[] = {"SAW", "PULSE", "TRI", "SINE"};
void hertz(int value, char *out) {           // CUTOFF 0-127 shown as a rough frequency
    int hz = 20 + value * value;
    int n = 0;
    char digits[8];
    int k = 0;
    do { digits[k++] = char('0' + hz % 10); hz /= 10; } while (hz);
    while (k) out[n++] = digits[--k];
    out[n++] = 'H'; out[n++] = 'Z'; out[n] = 0;
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

void log2(const char *a, const char *b) {
    char line[40];
    int n = 0;
    for (; *a && n < 38; a++) line[n++] = *a;
    if (n < 38) line[n++] = ' ';
    for (; *b && n < 39; b++) line[n++] = *b;
    line[n] = 0;
    enaga::log(line);
}
void log_number(const char *a, int value) {
    char text[12];
    int n = 0, k = 0;
    char digits[12];
    unsigned v = value < 0 ? unsigned(-value) : unsigned(value);
    do { digits[k++] = char('0' + v % 10); v /= 10; } while (v);
    if (value < 0) text[n++] = '-';
    while (k) text[n++] = digits[--k];
    text[n] = 0;
    log2(a, text);
}

}  // namespace

class Widgets : public enaga::App {
public:
    bool init() override {
        sounds_.set_title("SOUNDS");
        sounds_.set_items(kSounds, kSoundCount);
        for (int i = 0; i < kParamCount; i++) values_[i] = kParams[i].initial;
        edit_.set("EDIT", kParams, kParamCount, values_);
        copy(name_, "MY SOUND");
        return true;
    }

    void on_key(int key, bool down) override {
        if (dialog_.active()) {                   // an open dialog or text entry takes every key
            Action a = dialog_.key(key, down);
            if (a == Action::yes) {
                for (int i = 0; i < kParamCount; i++) values_[i] = kParams[i].initial;
                enaga::log("RESET YES");
            } else if (a == Action::no) {
                enaga::log("RESET NO");
            }
            if (!dialog_.active()) page_ = kEdit;
            return;
        }
        if (entry_.active()) {
            Action a = entry_.key(key, down);
            if (a == Action::chosen) { copy(name_, entry_.text()); log2("NAME", name_); }
            if (a == Action::no) enaga::log("NAME KEPT");
            if (!entry_.active()) page_ = kSounds_;
            return;
        }
        if (down && key == ENAGA_KEY_F2) { page_ = kSounds_; return; }
        if (down && key == ENAGA_KEY_F3) { page_ = kEdit; return; }
        if (down && key == ENAGA_KEY_F4) { entry_.open("NAME", name_, 12); page_ = kName; return; }
        if (down && key == ENAGA_KEY_F5) { dialog_.open("RESET ALL\nPARAMETERS?"); page_ = kReset; return; }
        if (page_ == kSounds_) {
            Action a = sounds_.key(key, down);
            if (a == Action::chosen) { chosen_ = sounds_.cursor(); log_number("CHOSE", chosen_); }
        } else if (page_ == kEdit) {
            if (edit_.key(key, down) == Action::changed) log_change();
        }
    }

    void on_encoder(int encoder, int delta) override {
        if (entry_.active()) { entry_.encoder(encoder, delta); return; }
        if (dialog_.active()) return;
        if (page_ == kSounds_) sounds_.encoder(encoder, delta);
        else if (page_ == kEdit && edit_.encoder(encoder, delta) == Action::changed) log_change();
    }

    void draw(enaga::Canvas &canvas) override {
        canvas.clear();
        canvas.text(0, 0, name_, 15);             // the header: the name and the chosen sound
        enaga::ui::text_right(canvas, 0, kSounds[chosen_], 8);
        canvas.rect(0, 9, enaga::ui::screen_width, 1, 3);
        const int top = 13;
        if (page_ == kSounds_) sounds_.draw(canvas, top, 118);
        else if (page_ == kEdit || page_ == kReset) edit_.draw(canvas, top);
        else if (page_ == kName) entry_.draw(canvas, top);
        dialog_.draw(canvas);                     // over the page, when open
        canvas.text(0, 120, "PAGES: F2 F3 F4 F5", 5);
    }

private:
    enum Page { kSounds_, kEdit, kName, kReset };

    void log_change() {
        char text[16];
        int n = 0, p = edit_.changed();
        text[n++] = char('0' + p);
        text[n++] = ' ';
        int v = values_[p];
        char digits[12];
        int k = 0;
        do { digits[k++] = char('0' + v % 10); v /= 10; } while (v);
        while (k) text[n++] = digits[--k];
        text[n] = 0;
        log2("CHANGED", text);
    }
    static void copy(char *out, const char *s) {
        int n = 0;
        for (; s[n] && n < 16; n++) out[n] = s[n];
        out[n] = 0;
    }

    enaga::ui::List sounds_;
    enaga::ui::ParamPage edit_;
    enaga::ui::TextEntry entry_;
    enaga::ui::Dialog dialog_;
    int values_[kParamCount] = {};
    char name_[17] = {};
    int chosen_ = 0;
    Page page_ = kSounds_;
};

ENAGA_APP(Widgets)
