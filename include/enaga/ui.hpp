// A small UI toolkit for apps: a list, a page of parameters on the knobs, a confirmation dialog and text entry,
// drawn on the Canvas with Enaga's 5 x 7 font so apps share a look (the Enaga menu's and the synth's).
//
// Each widget is driven from the app's own callbacks and returns what happened:
//
//     enaga::ui::List list;                            // members of the app
//     void on_key(int key, bool down) override {
//         if (list.key(key, down) == enaga::ui::Action::chosen) open(list.cursor());
//     }
//     void on_encoder(int encoder, int delta) override { list.encoder(encoder, delta); }
//     void draw(enaga::Canvas &canvas) override { canvas.clear(); list.draw(canvas); }
//
// Controls follow the original firmware and the synth: a page of parameters puts up to four on knobs A-D, the
// selection buttons under the screen flip between groups of four, and pushing a knob returns its parameter to its
// initial value; lists move with UP / DOWN or VALUE, OK chooses and CLR goes back. F1 stays the app's.
//
// Around the widgets: Line builds text without printf, status_line and hint_line frame the screen (the widgets go
// between body_top and body_bottom), Notice shows a message for a moment, and bar and meter draw levels.
//
// Widgets keep no pointers to the app beyond what it hands them (item names, parameters, values), which must
// outlive the widget. Main context only. Text is upper case: the font has A-Z, 0-9, space and ! # + - . / : = ? % >.
#pragma once

#include <enaga/app.hpp>
#include <enaga/dsp/math.h>

#include <cstdint>

namespace enaga::ui {

enum class Action {
    none,       // the event was not for this widget, or changed nothing
    moved,      // the cursor or group moved
    changed,    // a value changed (ParamPage::changed() says which)
    chosen,     // OK on a list entry, or text accepted
    back,       // CLR on a list
    yes,        // a dialog answered OK
    no,         // a dialog answered CLR
};

constexpr int screen_width = ENAGA_SCREEN_W;
constexpr int char_width = 6;                 // the font's advance
constexpr int line_chars = screen_width / char_width;   // 21

namespace detail {
inline int length(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
inline void copy(char *out, int size, const char *s) {
    int n = 0;
    while (s && s[n] && n < size - 1) { out[n] = s[n]; n++; }
    out[n] = 0;
}
inline void number(char *out, int value) {   // decimal, with a minus sign
    char digits[12];
    int n = 0, k = 0;
    unsigned v = value < 0 ? unsigned(-value) : unsigned(value);
    do { digits[n++] = char('0' + v % 10); v /= 10; } while (v);
    if (value < 0) out[k++] = '-';
    while (n) out[k++] = digits[--n];
    out[k] = 0;
}
inline void right(Canvas &canvas, int y, const char *text, uint8_t grey, int x_end = screen_width) {
    canvas.text(x_end - char_width * length(text), y, text, grey);
}
}  // namespace detail

// Text ending at x_end (the screen's right edge by default): values and labels in a column.
inline void text_right(Canvas &canvas, int y, const char *text, uint8_t grey = 15, int x_end = screen_width) {
    detail::right(canvas, y, text, grey, x_end);
}

// ---- Text -------------------------------------------------------------------------------------------------------

// Whether Enaga's font has a character; it draws any other as '?'.
inline bool drawable(char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
    for (const char *p = " !#+-./:=?%>"; *p; p++) if (*p == c) return true;
    return false;
}

// A line of text built in place, never longer than the screen's line (line_chars): whatever would run past the
// end is cut off. Each call returns the line, so calls chain:
//
//     enaga::ui::Line line;
//     line.add("TEMPO ").fixed(1300, 1).add(" BPM");           // "TEMPO 130.0 BPM"
//     canvas.text(0, 20, line);
class Line {
public:
    static constexpr int capacity = line_chars;

    Line() = default;
    explicit Line(const char *s) { add(s); }

    Line &clear() { n_ = 0; text_[0] = 0; return *this; }
    Line &add(char c) { if (n_ < capacity) { text_[n_++] = c; text_[n_] = 0; } return *this; }
    Line &add(const char *s) { while (s && *s) add(*s++); return *this; }

    // A number, right-aligned in `width` characters filled with `pad`: number(7, 3, '0') is "007".
    Line &number(int value, int width = 0, char pad = ' ') {
        char digits[12];
        int k = 0;
        unsigned v = value < 0 ? 0u - unsigned(value) : unsigned(value);
        do { digits[k++] = char('0' + v % 10); v /= 10; } while (v);
        int used = k + (value < 0);
        if (value < 0 && pad == '0') add('-');
        for (; used < width; used++) add(pad);
        if (value < 0 && pad != '0') add('-');
        while (k) add(digits[--k]);
        return *this;
    }
    // A fixed-point number: `value` in units of 10^-decimals. fixed(1300, 1) is "130.0", fixed(-5, 2) "-0.05".
    Line &fixed(int value, int decimals) {
        unsigned scale = 1;
        for (int i = 0; i < decimals; i++) scale *= 10;
        unsigned v = value < 0 ? 0u - unsigned(value) : unsigned(value);
        if (value < 0) add('-');
        number(int(v / scale));
        if (decimals > 0) { add('.'); number(int(v % scale), decimals, '0'); }
        return *this;
    }
    // `digits` hex digits, upper case: hex(0x3C, 2) is "3C".
    Line &hex(unsigned value, int digits) {
        for (int shift = 4 * (digits - 1); shift >= 0; shift -= 4) add("0123456789ABCDEF"[(value >> shift) & 15]);
        return *this;
    }
    // A MIDI note's name, with middle C (60) as C4: 61 is "C#4", 0 "C-1".
    Line &note(int note) {
        static const char names[] = "C C#D D#E F F#G G#A A#B ";
        int pitch = ((note % 12) + 12) % 12, octave = (note - pitch) / 12 - 1;
        add(names[2 * pitch]);
        if (names[2 * pitch + 1] != ' ') add(names[2 * pitch + 1]);
        return number(octave);
    }
    // Text from outside the app, a file name, say, as the font can draw it: lower case raised, anything else the
    // font lacks shown as '-', at most `max` characters. "kick_01.wav" becomes "KICK-01.WAV".
    Line &shown(const char *text, int max = capacity) {
        for (int i = 0; text && text[i] && i < max; i++) {
            char c = text[i] >= 'a' && text[i] <= 'z' ? char(text[i] - 'a' + 'A') : text[i];
            add(drawable(c) ? c : '-');
        }
        return *this;
    }
    // Adds `part` only if all of it fits, for a line made of optional pieces in order of importance: what does not
    // fit is left out whole rather than cut. Returns whether it was added.
    bool add_if_fits(const char *part) {
        if (n_ + detail::length(part) > capacity) return false;
        add(part);
        return true;
    }
    // Spaces up to character column `at` (0-20), to line values up under each other.
    Line &column(int at) { while (n_ < at && n_ < capacity) add(' '); return *this; }

    const char *text() const { return text_; }
    operator const char *() const { return text_; }
    // Into a buffer of `size` bytes, cut to fit: for a Param's format function, say.
    void copy_to(char *out, int size) const { detail::copy(out, size, text_); }
    int length() const { return n_; }
    int room() const { return capacity - n_; }

private:
    char text_[capacity + 1] = {};
    int n_ = 0;
};

// ---- The screen's frame -----------------------------------------------------------------------------------------

// A status line at the top with a rule under it, key hints on the bottom line, and the widgets between: pass
// body_top and body_bottom as their `top` and `bottom`.
constexpr int status_y = 0;
constexpr int body_top = 12;
constexpr int hint_y = ENAGA_SCREEN_H - 8;   // 120, the bottom line
constexpr int body_bottom = hint_y - 2;

// The top line: `left` bright, `right` (if any) dim at the right edge, and a rule under both.
inline void status_line(Canvas &canvas, const char *left, const char *right = nullptr) {
    canvas.text(0, status_y, left ? left : "", 15);
    if (right) detail::right(canvas, status_y, right, 8);
    canvas.rect(0, status_y + 9, screen_width, 1, 3);
}

// Key hints ("OK: LOAD  CLR: BACK") on the bottom line, or `row` lines above it (10 pixels each).
inline void hint_line(Canvas &canvas, const char *hints, int row = 0) {
    canvas.text(0, hint_y - 10 * row, hints ? hints : "", 6);
}

// ---- Notice -----------------------------------------------------------------------------------------------------

// A message shown for a moment ("SAVED", "NO CARD"): show() copies the text, and draw() puts it on a lit bar over
// the bottom line until the time is up. Drawn after the hints, it covers them.
class Notice {
public:
    static constexpr uint32_t default_ms = 1500;

    void show(const char *text, uint32_t duration_ms = default_ms) {
        detail::copy(text_, sizeof text_, text);
        since_ = enaga::ms();
        duration_ = duration_ms;
    }
    void clear() { text_[0] = 0; }
    bool showing() const { return text_[0] && enaga::ms() - since_ < duration_; }
    const char *text() const { return showing() ? text_ : ""; }

    void draw(Canvas &canvas, int y = hint_y) const {
        if (!showing()) return;
        canvas.rect(0, y - 2, screen_width, 10, 4);
        canvas.text(2, y, text_, 15);
    }

private:
    char text_[line_chars + 1] = {};
    uint32_t since_ = 0, duration_ = 0;
};

// ---- Bars and meters --------------------------------------------------------------------------------------------

namespace detail {
inline int place(int value, int min, int max, int pixels) {   // value's place in min..max, 0..pixels
    if (max <= min) return 0;
    value = value < min ? min : value > max ? max : value;
    unsigned range = unsigned(max) - unsigned(min), offset = unsigned(value) - unsigned(min);
    while (range > (1u << 22)) { range >>= 1; offset >>= 1; }   // 32-bit: no 64-bit division on the C674x
    return int(unsigned(pixels) * offset / range);
}
inline int fraction(float f, int pixels) {
    return f <= 0.0f ? 0 : f >= 1.0f ? pixels : int(f * float(pixels) + 0.5f);
}
}  // namespace detail

// A value's place in its range: a dim track `w` pixels long and `h` high, lit from the left up to the value.
inline void bar(Canvas &canvas, int x, int y, int w, int value, int min, int max, int h = 1, uint8_t grey = 9) {
    canvas.rect(x, y, w, h, 2);
    canvas.rect(x, y, detail::place(value, min, max, w), h, grey);
}

// The same lit from the middle of the range towards the value: for values either side of a centre (pan,
// detune). A mark shows the middle.
inline void bar_centred(Canvas &canvas, int x, int y, int w, int value, int min, int max, int h = 1,
                        uint8_t grey = 9) {
    canvas.rect(x, y, w, h, 2);
    int middle = w / 2, at = detail::place(value, min, max, w);
    if (at < middle) canvas.rect(x + at, y, middle - at, h, grey);
    else canvas.rect(x + middle, y, at - middle, h, grey);
    canvas.rect(x + middle, y - 1, 1, h + 2, 6);
}

// A level meter: upright, filling from the bottom, when it is taller than wide; otherwise across, from the left.
// `level` and `peak` are fractions of full scale, 0-1 (meter_db makes one from an amplitude); a peak at 0 or above
// shows as a bright mark.
inline void meter(Canvas &canvas, int x, int y, int w, int h, float level, float peak = -1.0f) {
    canvas.rect(x, y, w, h, 2);
    if (h > w) {
        int lit = detail::fraction(level, h);
        canvas.rect(x, y + h - lit, w, lit, 9);
        if (peak >= 0.0f) { int p = detail::fraction(peak, h - 1); canvas.rect(x, y + h - 1 - p, w, 1, 15); }
    } else {
        canvas.rect(x, y, detail::fraction(level, w), h, 9);
        if (peak >= 0.0f) canvas.rect(x + detail::fraction(peak, w - 1), y, 1, h, 15);
    }
}

// An amplitude (1.0 full scale) as a meter fraction on a decibel scale: floor_db and below are 0, 0 dB is 1.
inline float meter_db(float amplitude, float floor_db = -48.0f) {
    if (amplitude < 0.0f) amplitude = -amplitude;
    if (amplitude <= 0.0f || floor_db >= 0.0f) return 0.0f;
    float db = 6.0206f * enaga_log2(amplitude);
    return db <= floor_db ? 0.0f : db >= 0.0f ? 1.0f : 1.0f - db / floor_db;
}

// ---- List -------------------------------------------------------------------------------------------------------

// A scrolling list of entries with a title. UP / DOWN or VALUE move the cursor (it stops at the ends); OK gives
// Action::chosen, CLR Action::back. Entries are fixed strings, or computed by a function when they change.
class List {
public:
    using Label = void (*)(int index, char *out, int size, void *user);
    static constexpr int row_height = 11;

    void set_title(const char *title) { title_ = title; }
    void set_items(const char *const *names, int count) { names_ = names; label_ = nullptr; set_count(count); }
    void set_items(int count, Label label, void *user) { names_ = nullptr; label_ = label; user_ = user; set_count(count); }
    void set_count(int count) { count_ = count < 0 ? 0 : count; select(cursor_); }

    int count() const { return count_; }
    int cursor() const { return cursor_; }
    void select(int index) { cursor_ = index >= count_ ? count_ - 1 : index < 0 ? 0 : index; }

    Action key(int key, bool down) {
        if (!down) return Action::none;
        if (key == ENAGA_KEY_UP) return move(-1);
        if (key == ENAGA_KEY_DOWN) return move(1);
        if (key == ENAGA_KEY_OK) return count_ ? Action::chosen : Action::none;
        if (key == ENAGA_KEY_CLR) return Action::back;
        return Action::none;
    }
    Action encoder(int encoder, int delta) { return encoder == 4 ? move(delta) : Action::none; }

    // Draws from `top` to `bottom`: the title, then as many rows as fit, the cursor's on a dim bar.
    void draw(Canvas &canvas, int top = 0, int bottom = ENAGA_SCREEN_H) const {
        int y = top;
        if (title_) { canvas.text(0, y, title_, 15); y += 12; }
        int rows = (bottom - y) / row_height;
        if (rows < 1 || !count_) { if (!count_) canvas.text(9, y + 2, "EMPTY", 6); return; }
        int first = cursor_ - rows / 2;
        first = first > count_ - rows ? count_ - rows : first;
        first = first < 0 ? 0 : first;
        char text[line_chars + 1];
        for (int row = 0; row < rows && first + row < count_; row++) {
            int n = first + row, ry = y + row * row_height;
            if (n == cursor_) canvas.rect(0, ry, screen_width - 4, row_height - 1, 3);
            name(n, text, sizeof text - 3);
            canvas.text(3, ry + 2, text, n == cursor_ ? 15 : 10);
        }
        if (first > 0) canvas.rect(screen_width - 2, y, 2, 3, 8);                          // more above
        if (first + rows < count_) canvas.rect(screen_width - 2, y + rows * row_height - 4, 2, 3, 8);   // and below
    }

    void name(int index, char *out, int size) const {
        if (label_) { out[0] = 0; label_(index, out, size, user_); }
        else detail::copy(out, size, names_ ? names_[index] : "");
    }

private:
    Action move(int delta) {
        int before = cursor_;
        select(cursor_ + delta);
        return cursor_ != before ? Action::moved : Action::none;
    }
    const char *title_ = nullptr;
    const char *const *names_ = nullptr;
    Label label_ = nullptr;
    void *user_ = nullptr;
    int count_ = 0, cursor_ = 0;
};

// ---- ParamPage --------------------------------------------------------------------------------------------------

// One parameter: a name (up to 12 characters show), a range, the value a knob push returns to, and how to show the
// value: names for each step from min (`labels`), a function, or the number with an optional unit.
struct Param {
    const char *name;
    int min, max, initial;
    const char *const *labels = nullptr;      // labels[value - min]
    void (*format)(int value, char *out) = nullptr;   // out holds at least 12 characters
    const char *unit = nullptr;               // after the number: "MS", "%"
};

// Parameters on the knobs: the page's parameters in groups of four, in order, A-D on knobs A-D. The selection
// buttons flip between groups; turning a knob changes its parameter (clamped); pushing it returns the parameter to
// its initial value. Values live in the app's array, one per parameter, and the page only changes them.
class ParamPage {
public:
    ParamPage() = default;
    ParamPage(const char *title, const Param *params, int count, int *values) { set(title, params, count, values); }
    void set(const char *title, const Param *params, int count, int *values) {
        title_ = title; params_ = params; count_ = count; values_ = values; group_ = 0; changed_ = -1;
    }

    int groups() const { return (count_ + 3) / 4; }
    int group() const { return group_; }
    void show_group(int group) { group_ = group < 0 ? 0 : group >= groups() ? groups() - 1 : group; }
    int changed() const { return changed_; }  // the parameter the last Action::changed was for
    int value(int param) const { return values_[param]; }

    // Sets a value, clamped to its range; returns true if it changed.
    bool set_value(int param, int value) {
        const Param &p = params_[param];
        value = value < p.min ? p.min : value > p.max ? p.max : value;
        if (values_[param] == value) return false;
        values_[param] = value;
        changed_ = param;
        return true;
    }

    Action key(int key, bool down) {
        if (!down || !count_) return Action::none;
        if (key == ENAGA_KEY_SELECT_LEFT || key == ENAGA_KEY_SELECT_RIGHT) {
            if (groups() < 2) return Action::none;
            group_ = (group_ + (key == ENAGA_KEY_SELECT_RIGHT ? 1 : groups() - 1)) % groups();
            return Action::moved;
        }
        int knob = key == ENAGA_KEY_PUSH_A ? 0 : key == ENAGA_KEY_PUSH_B ? 1 : key == ENAGA_KEY_PUSH_C ? 2
                 : key == ENAGA_KEY_PUSH_D ? 3 : -1;
        int param = knob < 0 ? -1 : group_ * 4 + knob;
        if (param < 0 || param >= count_) return Action::none;
        return set_value(param, params_[param].initial) ? Action::changed : Action::none;
    }

    Action encoder(int encoder, int delta) {
        int param = group_ * 4 + encoder;
        if (encoder > 3 || param >= count_) return Action::none;   // VALUE and knobs past the group's end
        return set_value(param, values_[param] + delta) ? Action::changed : Action::none;
    }

    // The value as shown: a label, the format function's text, or the number and unit.
    void format(int param, char *out) const {
        const Param &p = params_[param];
        int v = values_[param];
        if (p.labels) detail::copy(out, 13, p.labels[v - p.min]);
        else if (p.format) { out[0] = 0; p.format(v, out); }
        else {
            detail::number(out, v);
            if (p.unit) detail::copy(out + detail::length(out), 13 - detail::length(out), p.unit);
        }
    }

    // The title and the group's place ("FILTER 2/3"), then a row per knob: its letter, the name with a bar under it
    // for the value's place in its range, and the value at the right (dimmer when it is not the initial value).
    void draw(Canvas &canvas, int top = 0) const {
        if (!count_) return;
        char text[line_chars + 1];
        canvas.text(0, top, title_ ? title_ : "", 15);
        if (groups() > 1) {
            detail::number(text, group_ + 1);
            int n = detail::length(text);
            text[n++] = '/';
            detail::number(text + n, groups());
            detail::right(canvas, top, text, 6);
        }
        for (int knob = 0; knob < 4; knob++) {
            int param = group_ * 4 + knob;
            if (param >= count_) break;
            const Param &p = params_[param];
            int y = top + 12 + 13 * knob, v = values_[param];
            char letter[2] = {char('A' + knob), 0};
            canvas.text(0, y, letter, 6);
            detail::copy(text, 13, p.name);
            canvas.text(9, y, text, 12);
            format(param, text);
            detail::right(canvas, y, text, v == p.initial ? 11 : 15);
            bar(canvas, 9, y + 8, 40, v, p.min, p.max);
        }
    }

private:
    const char *title_ = nullptr;
    const Param *params_ = nullptr;
    int count_ = 0, group_ = 0, changed_ = -1;
    int *values_ = nullptr;
};

// ---- Dialog -----------------------------------------------------------------------------------------------------

// A question in a box over whatever is drawn: up to three lines (split at '\n', 19 characters each) and the
// answers. While it is open, key() takes every key: OK gives Action::yes, CLR Action::no, and both close it.
class Dialog {
public:
    void open(const char *message, const char *yes = "OK", const char *no = "CLR") {
        message_ = message; yes_ = yes; no_ = no; open_ = true;
    }
    void close() { open_ = false; }
    bool active() const { return open_; }

    Action key(int key, bool down) {
        if (!open_) return Action::none;
        if (down && key == ENAGA_KEY_OK) { open_ = false; return Action::yes; }
        if (down && key == ENAGA_KEY_CLR) { open_ = false; return Action::no; }
        return Action::moved;                 // swallowed while open
    }
    Action encoder(int, int) { return Action::none; }

    void draw(Canvas &canvas) const {
        if (!open_) return;
        const int x = 4, w = screen_width - 8, y = 32, h = 64;
        canvas.rect(x, y, w, h, 0);
        canvas.rect(x, y, w, 1, 12); canvas.rect(x, y + h - 1, w, 1, 12);
        canvas.rect(x, y, 1, h, 12); canvas.rect(x + w - 1, y, 1, h, 12);
        char line[20];
        const char *s = message_ ? message_ : "";
        for (int row = 0; row < 3 && *s; row++) {
            int n = 0;
            while (s[n] && s[n] != '\n' && n < 19) n++;
            for (int i = 0; i < n; i++) line[i] = s[i];
            line[n] = 0;
            canvas.text(x + 4, y + 6 + 11 * row, line, 15);
            s += n;
            if (*s == '\n') s++;
        }
        char answers[line_chars + 1];
        int n = 0;
        auto add = [&](const char *t) { while (*t && n < line_chars) answers[n++] = *t++; };
        add(yes_); add(":YES  "); add(no_); add(":NO");
        answers[n] = 0;
        canvas.text(x + 4, y + h - 12, answers, 8);
    }

private:
    const char *message_ = nullptr, *yes_ = "OK", *no_ = "CLR";
    bool open_ = false;
};

// ---- TextEntry --------------------------------------------------------------------------------------------------

// Editing a short name (up to max_chars). LEFT / RIGHT or knob A move the cursor; VALUE, knob B or UP / DOWN change
// the character under it, through space, A-Z, 0-9 and - + . # !; OK accepts (Action::chosen) and CLR cancels
// (Action::no), both closing it. text() is the result, trailing spaces removed. While open, key() takes every key.
class TextEntry {
public:
    static constexpr int max_chars = 16;
    static constexpr const char *characters = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-+.#!";

    void open(const char *title, const char *initial, int chars = max_chars) {
        title_ = title;
        chars_ = chars < 1 ? 1 : chars > max_chars ? max_chars : chars;
        int n = 0;
        for (; initial && initial[n] && n < chars_; n++) text_[n] = known(initial[n]);
        for (; n < chars_; n++) text_[n] = ' ';
        text_[chars_] = 0;
        cursor_ = 0;
        open_ = true;
    }
    bool active() const { return open_; }

    // The text without trailing spaces (valid after open; after OK it is the answer).
    const char *text() {
        int n = chars_;
        while (n > 0 && text_[n - 1] == ' ') n--;
        detail::copy(result_, sizeof result_, text_);
        result_[n] = 0;
        return result_;
    }
    int cursor() const { return cursor_; }

    Action key(int key, bool down) {
        if (!open_) return Action::none;
        if (!down) return Action::moved;
        switch (key) {
        case ENAGA_KEY_LEFT: return move(-1);
        case ENAGA_KEY_RIGHT: return move(1);
        case ENAGA_KEY_UP: return step(1);
        case ENAGA_KEY_DOWN: return step(-1);
        case ENAGA_KEY_OK: open_ = false; return Action::chosen;
        case ENAGA_KEY_CLR: open_ = false; return Action::no;
        default: return Action::moved;        // swallowed while open
        }
    }
    Action encoder(int encoder, int delta) {
        if (!open_) return Action::none;
        if (encoder == 0) return move(delta);
        if (encoder == 1 || encoder == 4) return step(delta);
        return Action::none;
    }

    // The title, the text in boxes with the cursor's box lit, and the controls.
    void draw(Canvas &canvas, int top = 0) const {
        if (!open_) return;
        canvas.text(0, top, title_ ? title_ : "", 15);
        int x0 = (screen_width - chars_ * 7) / 2, y = top + 30;
        for (int i = 0; i < chars_; i++) {
            int x = x0 + i * 7;
            canvas.rect(x, y + 9, 6, 1, i == cursor_ ? 15 : 4);
            if (i == cursor_) canvas.rect(x - 1, y - 2, 8, 11, 4);
            char c[2] = {text_[i], 0};
            canvas.text(x, y, c, i == cursor_ ? 15 : 11);
        }
        canvas.text(0, top + 56, "A / LEFT RIGHT: MOVE", 6);
        canvas.text(0, top + 66, "B / VALUE: CHARACTER", 6);
        canvas.text(0, top + 76, "OK: DONE  CLR: CANCEL", 6);
    }

private:
    static char known(char c) {
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        for (const char *p = characters; *p; p++) if (*p == c) return c;
        return ' ';
    }
    Action move(int delta) {
        int before = cursor_;
        cursor_ += delta;
        cursor_ = cursor_ < 0 ? 0 : cursor_ >= chars_ ? chars_ - 1 : cursor_;
        return cursor_ != before ? Action::moved : Action::none;
    }
    Action step(int delta) {
        int count = detail::length(characters), at = 0;
        while (characters[at] && characters[at] != text_[cursor_]) at++;
        at = ((at + delta) % count + count) % count;
        text_[cursor_] = characters[at];
        return Action::changed;
    }
    const char *title_ = nullptr;
    char text_[max_chars + 1] = {}, result_[max_chars + 1] = {};
    int chars_ = max_chars, cursor_ = 0;
    bool open_ = false;
};

}  // namespace enaga::ui
