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
#include <enaga/files.hpp>

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

// ---- Grid -------------------------------------------------------------------------------------------------------

// Cells in rows and columns, such as a bank of patterns or sound slots: a List in two dimensions. The arrow keys
// move the cursor (stopping at the edges), VALUE moves it through the cells in order; OK gives Action::chosen and
// CLR Action::back. A cell can be lit, drawn filled: a slot that holds something, a pattern that is playing. Labels
// are fixed strings or computed, as in List; as many characters show as fit the cell. Up to max_cells cells.
class Grid {
public:
    static constexpr int max_cells = 64;
    static constexpr int cell_height = 12;
    using Label = void (*)(int index, char *out, int size, void *user);

    void set_title(const char *title) { title_ = title; }
    void set_items(const char *const *names, int count, int columns) {
        names_ = names; label_ = nullptr; set_shape(count, columns);
    }
    void set_items(int count, int columns, Label label, void *user) {
        names_ = nullptr; label_ = label; user_ = user; set_shape(count, columns);
    }

    int count() const { return count_; }
    int columns() const { return columns_; }
    int cursor() const { return cursor_; }
    void select(int cell) { cursor_ = cell >= count_ ? count_ - 1 : cell < 0 ? 0 : cell; }
    void light(int cell, bool on) {
        if (cell < 0 || cell >= max_cells) return;
        uint64_t bit = uint64_t(1) << cell;
        lit_ = on ? lit_ | bit : lit_ & ~bit;
    }
    bool lit(int cell) const { return cell >= 0 && cell < max_cells && (lit_ >> cell & 1); }
    void clear_lights() { lit_ = 0; }

    Action key(int key, bool down) {
        if (!down || !count_) return Action::none;
        int column = cursor_ % columns_;
        switch (key) {
        case ENAGA_KEY_LEFT: return column > 0 ? to(cursor_ - 1) : Action::none;
        case ENAGA_KEY_RIGHT: return column < columns_ - 1 ? to(cursor_ + 1) : Action::none;
        case ENAGA_KEY_UP: return to(cursor_ - columns_);
        case ENAGA_KEY_DOWN: return to(cursor_ + columns_);
        case ENAGA_KEY_OK: return Action::chosen;
        case ENAGA_KEY_CLR: return Action::back;
        default: return Action::none;
        }
    }
    Action encoder(int encoder, int delta) {
        if (encoder != 4 || !count_) return Action::none;
        int before = cursor_;
        select(cursor_ + delta);
        return cursor_ != before ? Action::moved : Action::none;
    }

    void name(int index, char *out, int size) const {
        if (label_) { out[0] = 0; label_(index, out, size, user_); }
        else detail::copy(out, size, names_ ? names_[index] : "");
    }

    // The title, then as many rows of cells as fit, scrolled to keep the cursor's row in view. Each cell is its
    // share of the width: lit cells filled, the cursor's framed.
    void draw(Canvas &canvas, int top = 0, int bottom = ENAGA_SCREEN_H) const {
        int y = top;
        if (title_) { canvas.text(0, y, title_, 15); y += 12; }
        if (!count_) { canvas.text(9, y + 2, "EMPTY", 6); return; }
        int rows = (bottom - y) / cell_height, total = (count_ + columns_ - 1) / columns_;
        if (rows < 1) return;
        int row = cursor_ / columns_, first = row < rows ? 0 : row - rows + 1;
        if (first > total - rows) first = total - rows > 0 ? total - rows : 0;
        int w = screen_width / columns_, chars = (w - 4) / char_width;
        char text[line_chars + 1];
        for (int r = 0; r < rows && first + r < total; r++) {
            for (int c = 0; c < columns_; c++) {
                int cell = (first + r) * columns_ + c;
                if (cell >= count_) break;
                int x = c * w, cy = y + r * cell_height;
                bool on = lit(cell), here = cell == cursor_;
                canvas.rect(x + 1, cy, w - 2, cell_height - 1, on ? 9 : 2);
                if (here) {
                    canvas.rect(x, cy - 1, w, 1, 15); canvas.rect(x, cy + cell_height - 1, w, 1, 15);
                    canvas.rect(x, cy - 1, 1, cell_height + 1, 15); canvas.rect(x + w - 1, cy - 1, 1, cell_height + 1, 15);
                }
                name(cell, text, chars + 1 < int(sizeof text) ? chars + 1 : int(sizeof text));
                canvas.text(x + 2, cy + 2, text, on ? 0 : here ? 15 : 10);
            }
        }
        if (first > 0) canvas.rect(screen_width - 2, y, 2, 3, 8);
        if (first + rows < total) canvas.rect(screen_width - 2, y + rows * cell_height - 4, 2, 3, 8);
    }

private:
    void set_shape(int count, int columns) {
        count_ = count < 0 ? 0 : count > max_cells ? max_cells : count;
        columns_ = columns < 1 ? 1 : columns;
        select(cursor_);
    }
    Action to(int cell) {
        if (cell < 0 || cell >= count_) return Action::none;
        cursor_ = cell;
        return Action::moved;
    }
    const char *title_ = nullptr;
    const char *const *names_ = nullptr;
    Label label_ = nullptr;
    void *user_ = nullptr;
    int count_ = 0, columns_ = 1, cursor_ = 0;
    uint64_t lit_ = 0;
};

// ---- PadMap -----------------------------------------------------------------------------------------------------

// The fifteen pads as they lie on the panel, for a screen that says what each pad does: two staggered rows like a
// piano's keys, eight below (the even pads 0-14, left to right) and seven above (the odd pads 1-13), each upper pad
// over the gap between two lower ones (docs/hardware.md "Panel MCU"). Each pad has a short label (two characters
// show), a level of light (0 dark to 15) and one pad can be marked as selected. The app sets them, from on_pad_press
// for instance: the map takes no events.
class PadMap {
public:
    static constexpr int pads = 15;
    static constexpr int cell_width = 16, cell_height = 14;
    static constexpr int height = 2 * cell_height;

    static bool upper(int pad) { return pad % 2 == 1; }
    static int x(int pad) { return cell_width * (pad / 2) + (upper(pad) ? cell_width / 2 : 0); }

    void label(int pad, const char *text) { if (valid(pad)) detail::copy(labels_[pad], 3, text); }
    void light(int pad, uint8_t grey) { if (valid(pad)) light_[pad] = grey > 15 ? 15 : grey; }
    void select(int pad) { selected_ = valid(pad) ? pad : -1; }
    int selected() const { return selected_; }

    // The upper row at `top`, the lower one under it; `height` pixels in all.
    void draw(Canvas &canvas, int top) const {
        for (int pad = 0; pad < pads; pad++) {
            int px = x(pad), py = top + (upper(pad) ? 0 : cell_height);
            uint8_t fill = light_[pad] < 2 ? 2 : light_[pad];
            canvas.rect(px + 1, py + 1, cell_width - 2, cell_height - 2, fill);
            if (pad == selected_) {
                canvas.rect(px, py, cell_width, 1, 15); canvas.rect(px, py + cell_height - 1, cell_width, 1, 15);
                canvas.rect(px, py, 1, cell_height, 15); canvas.rect(px + cell_width - 1, py, 1, cell_height, 15);
            }
            canvas.text(px + 2, py + 4, labels_[pad], fill >= 8 ? 0 : 15);
        }
    }

private:
    static bool valid(int pad) { return pad >= 0 && pad < pads; }
    char labels_[pads][3] = {};
    uint8_t light_[pads] = {};
    int selected_ = -1;
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
    // A name for each group (groups() of them), shown as tabs on the title line in place of "1/3".
    void set_group_names(const char *const *names) { group_names_ = names; }
    // Columns: the four parameters side by side, each with its name, value and bar, rather than in rows. Names and
    // values then show five characters each.
    void set_columns(bool columns) { columns_ = columns; }

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

    // The title and the group's place ("FILTER 2/3", or the group names as tabs, the current one lit), then a row
    // per knob: its letter, the name with a bar under it for the value's place in its range, and the value at the
    // right (brighter when it is not the initial value). In columns, the same four side by side.
    void draw(Canvas &canvas, int top = 0) const {
        if (!count_) return;
        char text[line_chars + 1];
        canvas.text(0, top, title_ ? title_ : "", 15);
        if (groups() > 1) draw_groups(canvas, top);
        if (columns_) { draw_columns(canvas, top); return; }
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
    // Tabs when the groups have names and they fit beside the title, else "2/3".
    void draw_groups(Canvas &canvas, int top) const {
        int chars = 0;
        if (group_names_) for (int g = 0; g < groups(); g++) chars += detail::length(group_names_[g]) + 1;
        if (group_names_ && detail::length(title_) + chars <= line_chars) {
            int x = screen_width - (chars - 1) * char_width;
            for (int g = 0; g < groups(); g++) {
                int w = detail::length(group_names_[g]) * char_width;
                if (g == group_) canvas.rect(x - 1, top - 1, w + 1, 9, 4);
                canvas.text(x, top, group_names_[g], g == group_ ? 15 : 6);
                x += w + char_width;
            }
            return;
        }
        Line place;
        place.number(group_ + 1).add('/').number(groups());
        detail::right(canvas, top, place, 6);
    }

    void draw_columns(Canvas &canvas, int top) const {
        constexpr int w = screen_width / 4;
        char text[line_chars + 1];
        for (int knob = 0; knob < 4; knob++) {
            int param = group_ * 4 + knob;
            if (param >= count_) break;
            const Param &p = params_[param];
            int x = knob * w, y = top + 14, v = values_[param];
            char letter[2] = {char('A' + knob), 0};
            canvas.text(x, y, letter, 6);
            detail::copy(text, 6, p.name);
            canvas.text(x, y + 11, text, 12);
            format(param, text);
            text[5] = 0;
            canvas.text(x, y + 24, text, v == p.initial ? 11 : 15);
            bar(canvas, x, y + 34, w - 4, v, p.min, p.max, 2);
        }
    }

    const char *title_ = nullptr;
    const char *const *group_names_ = nullptr;
    const Param *params_ = nullptr;
    int count_ = 0, group_ = 0, changed_ = -1;
    int *values_ = nullptr;
    bool columns_ = false;
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

// ---- FileBrowser ------------------------------------------------------------------------------------------------

// Choosing a file on the card: a List of a folder's entries, folders first (shown ending in '/') and then files,
// each sorted by name. OK on a folder goes into it and CLR goes back up; OK on a file gives Action::chosen, and
// path() is then its full path. CLR in the starting folder gives Action::back. An extension (".WAV") shows only
// those files, in any case. Listing runs in the background through the file service: draw() says LOADING until it
// is done, and an error or an empty folder says so too. Up to max_entries entries a folder; names longer than
// name_bytes - 1 bytes are left out (they could not be opened from here). Main context only; keep the browser for
// as long as a listing may still be running (a member of the app).
class FileBrowser {
public:
    static constexpr int max_entries = 128, name_bytes = 64, path_bytes = 256;

    // Starts at `folder` ("/SAMPLES"), which is also as far up as CLR goes.
    void open(const char *folder, const char *extension = nullptr) {
        detail::copy(root_, path_bytes, folder);
        extension_ = extension;
        enter(folder);
    }

    bool loading() const { return pending_ > 0; }
    int32_t error() const { return error_; }           // the listing's error, or ENAGA_FILE_OK
    const char *folder() const { return folder_; }
    const char *path() const { return path_; }         // the chosen file, after Action::chosen
    int count() const { return count_; }
    int cursor() const { return list_.cursor(); }
    bool is_folder(int index) const { return index >= 0 && index < count_ && entries_[order_[index]].folder; }
    const char *name(int index) const { return index >= 0 && index < count_ ? entries_[order_[index]].name : ""; }

    Action key(int key, bool down) {
        if (!down) return Action::none;
        if (key == ENAGA_KEY_CLR) {
            if (same(folder_, root_)) return Action::back;
            up();
            return Action::moved;
        }
        if (key == ENAGA_KEY_OK) {
            if (loading() || !count_) return Action::none;
            int at = list_.cursor();
            if (!join(path_, folder_, name(at))) return Action::none;
            if (is_folder(at)) { char next[path_bytes]; detail::copy(next, path_bytes, path_); enter(next); return Action::moved; }
            return Action::chosen;
        }
        return loading() ? Action::none : list_.key(key, down);
    }
    Action encoder(int encoder, int delta) { return loading() ? Action::none : list_.encoder(encoder, delta); }

    // The folder (its end, if long) as the title, then its entries.
    void draw(Canvas &canvas, int top = 0, int bottom = ENAGA_SCREEN_H) const {
        Line title;
        int n = detail::length(folder_);
        title.shown(n > line_chars ? folder_ + n - line_chars : folder_);
        canvas.text(0, top, title, 15);
        if (loading()) { canvas.text(9, top + 14, "LOADING", 6); return; }
        if (error_ != ENAGA_FILE_OK) { canvas.text(9, top + 14, enaga::files::error_name(error_), 6); return; }
        list_.draw(canvas, top + 12, bottom);
    }

private:
    struct Entry { char name[name_bytes]; bool folder; };

    static bool same(const char *a, const char *b) {
        while (*a && *a == *b) { a++; b++; }
        return *a == *b;
    }
    static char upper(char c) { return c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c; }
    static bool before(const Entry &a, const Entry &b) {   // folders first, then by name, ignoring case
        if (a.folder != b.folder) return a.folder;
        for (int i = 0;; i++) {
            char x = upper(a.name[i]), y = upper(b.name[i]);
            if (x != y || !x) return x < y;
        }
    }
    bool wanted(const char *name) const {
        if (!extension_) return true;
        int n = detail::length(name), e = detail::length(extension_);
        if (n < e) return false;
        for (int i = 0; i < e; i++) if (upper(name[n - e + i]) != upper(extension_[i])) return false;
        return true;
    }
    static bool join(char *out, const char *folder, const char *name) {
        int n = detail::length(folder), m = detail::length(name), slash = n > 0 && folder[n - 1] == '/' ? 0 : 1;
        if (n + slash + m >= path_bytes) return false;
        detail::copy(out, path_bytes, folder);
        if (slash) out[n] = '/';
        detail::copy(out + n + slash, path_bytes - n - slash, name);
        return true;
    }

    void enter(const char *folder) {
        detail::copy(folder_, path_bytes, folder);
        count_ = 0;
        error_ = ENAGA_FILE_OK;
        list_.set_items(0, &label, this);
        stale_ = pending_;                       // entries of a listing still running are not this folder's
        int32_t queued = enaga::files::list<&FileBrowser::entry, &FileBrowser::done>(folder_, this);
        if (queued < 0) error_ = queued;
        else pending_++;
    }
    void up() {
        int n = detail::length(folder_);
        while (n > 1 && folder_[n - 1] == '/') n--;
        while (n > 1 && folder_[n - 1] != '/') n--;
        if (n > 1) n--;                          // "/SAMPLES/KICKS" -> "/SAMPLES", "/SAMPLES" -> "/"
        char parent[path_bytes];
        detail::copy(parent, n + 1, folder_);
        enter(parent);
    }

    void entry(const enaga::files::Info &info) {
        if (stale_ > 0 || count_ >= max_entries) return;
        bool folder = info.flags & ENAGA_FILE_FOLDER;
        if (detail::length(info.name) >= name_bytes || (!folder && !wanted(info.name))) return;
        Entry &e = entries_[count_];
        detail::copy(e.name, name_bytes, info.name);
        e.folder = folder;
        int at = count_++;                       // insertion into the sorted order
        while (at > 0 && before(e, entries_[order_[at - 1]])) { order_[at] = order_[at - 1]; at--; }
        order_[at] = uint8_t(count_ - 1);
    }
    void done(int32_t result) {
        if (pending_ > 0) pending_--;
        if (stale_ > 0) { stale_--; return; }
        if (result < 0) error_ = result;
        list_.set_items(count_, &label, this);
        list_.select(0);
    }
    static void label(int index, char *out, int size, void *user) {
        const FileBrowser *self = static_cast<const FileBrowser *>(user);
        Line text;
        text.shown(self->name(index));
        if (self->is_folder(index)) text.add('/');
        detail::copy(out, size, text);
    }

    Entry entries_[max_entries];
    uint8_t order_[max_entries] = {};
    char root_[path_bytes] = {}, folder_[path_bytes] = {}, path_[path_bytes] = {};
    const char *extension_ = nullptr;
    List list_;
    int count_ = 0, pending_ = 0, stale_ = 0;
    int32_t error_ = ENAGA_FILE_OK;
};

}  // namespace enaga::ui
