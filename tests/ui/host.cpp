// The UI toolkit (include/enaga/ui.hpp) on the host, with a stand-in for Enaga's host table that records the text
// each widget draws. Run by tests/test_ui.py; prints one line per failed check and exits non-zero if any failed.
#include <enaga/ui.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace enaga::ui;
using enaga::ui::Action;

namespace {
struct Drawn { int x, y; std::string text; int grey; };
std::vector<Drawn> g_drawn;
void host_text(uint8_t *, int x, int y, const char *text, uint8_t grey) { g_drawn.push_back({x, y, text, grey}); }
uint32_t g_ms = 1000;
uint32_t host_ms() { return g_ms; }
enaga_host_t g_host = [] { enaga_host_t h{}; h.text = host_text; h.ms = host_ms; return h; }();
uint8_t g_pixels[ENAGA_SCREEN_W * ENAGA_SCREEN_H];

int failures = 0;
void check(bool ok, const char *what) {
    if (!ok) { std::printf("FAIL %s\n", what); failures++; }
}
// The text drawn at row y (exactly), or "" if none; several on one row are joined with '|', left to right.
std::string row(int y) {
    std::string out;
    for (const Drawn &d : g_drawn) if (d.y == y) out += (out.empty() ? "" : "|") + d.text;
    return out;
}
const Drawn *find(const char *text) {
    for (const Drawn &d : g_drawn) if (d.text == text) return &d;
    return nullptr;
}
void draw_list(const List &list) {
    g_drawn.clear();
    enaga::Canvas canvas(g_pixels);
    list.draw(canvas);
}
}  // namespace

const enaga_host_t &enaga::host() { return g_host; }

static void test_list() {
    static const char *const names[] = {"ALPHA", "BRAVO", "CHARLIE"};
    List list;
    list.set_title("PICK");
    list.set_items(names, 3);
    check(list.key(ENAGA_KEY_UP, true) == Action::none, "list: up at the top does nothing");
    check(list.key(ENAGA_KEY_DOWN, true) == Action::moved && list.cursor() == 1, "list: down moves");
    check(list.key(ENAGA_KEY_DOWN, false) == Action::none, "list: key releases are ignored");
    check(list.encoder(4, 5) == Action::moved && list.cursor() == 2, "list: VALUE moves, stopping at the end");
    check(list.encoder(0, 1) == Action::none, "list: knob A is not the list's");
    check(list.key(ENAGA_KEY_OK, true) == Action::chosen, "list: OK chooses");
    check(list.key(ENAGA_KEY_CLR, true) == Action::back, "list: CLR goes back");
    draw_list(list);
    check(row(0) == "PICK", "list: title");
    const Drawn *c = find("CHARLIE");
    check(c && c->grey == 15, "list: the cursor's entry is bright");
    const Drawn *a = find("ALPHA");
    check(a && a->grey == 10, "list: other entries are dim");

    // A long, computed list scrolls to keep the cursor in view.
    List many;
    many.set_items(40, [](int i, char *out, int size, void *) { std::snprintf(out, size, "ITEM %d", i); }, nullptr);
    many.select(30);
    draw_list(many);
    check(find("ITEM 30") != nullptr, "list: the cursor stays in view");
    check(find("ITEM 0") == nullptr, "list: scrolled past the top");
    many.select(99);
    check(many.cursor() == 39, "list: select clamps");
    draw_list(many);
    check(find("ITEM 39") != nullptr && find("ITEM 39")->grey == 15, "list: the last entry at the bottom");
    many.set_count(10);
    check(many.cursor() == 9, "list: a shorter list moves the cursor in");
    List empty;
    check(empty.key(ENAGA_KEY_OK, true) == Action::none, "list: OK on an empty list does nothing");
    draw_list(empty);
    check(find("EMPTY") != nullptr, "list: an empty list says so");
}

static void cents(int v, char *out) { std::snprintf(out, 13, "%+d CT", v); }

static void test_param_page() {
    static const char *const waves[] = {"SAW", "PULSE", "SINE"};
    static const Param params[] = {
        {"WAVE", 0, 2, 0, waves},
        {"DETUNE", -50, 50, 0, nullptr, cents},
        {"ATTACK", 0, 2000, 10, nullptr, nullptr, "MS"},
        {"LEVEL", 0, 100, 80},
        {"CUTOFF", 0, 127, 64},
        {"RESONANCE", 0, 127, 0},
    };
    int values[6] = {0, 0, 10, 80, 64, 0};
    ParamPage page("SYNTH", params, 6, values);
    check(page.groups() == 2, "params: six make two groups");
    check(page.encoder(0, 1) == Action::changed && values[0] == 1 && page.changed() == 0, "params: knob A turns A");
    check(page.encoder(0, 10) == Action::changed && values[0] == 2, "params: clamped at max");
    check(page.encoder(0, 1) == Action::none, "params: no change at max");
    check(page.encoder(1, -100) == Action::changed && values[1] == -50, "params: clamped at min");
    check(page.encoder(4, 1) == Action::none, "params: VALUE is not the page's");
    check(page.key(ENAGA_KEY_PUSH_B, true) == Action::changed && values[1] == 0, "params: a push resets");
    check(page.key(ENAGA_KEY_PUSH_B, true) == Action::none, "params: a push at the initial value does nothing");
    char text[16];
    page.format(0, text); check(std::strcmp(text, "SINE") == 0, "params: labels");
    page.encoder(1, 7);
    page.format(1, text); check(std::strcmp(text, "+7 CT") == 0, "params: a format function");
    page.format(2, text); check(std::strcmp(text, "10MS") == 0, "params: number and unit");
    check(page.key(ENAGA_KEY_SELECT_RIGHT, true) == Action::moved && page.group() == 1, "params: select flips");
    check(page.encoder(2, 1) == Action::none, "params: knob C past the last group's end does nothing");
    check(page.encoder(1, 3) == Action::changed && values[5] == 3, "params: knob B in group 2 is the sixth");
    check(page.key(ENAGA_KEY_SELECT_RIGHT, true) == Action::moved && page.group() == 0, "params: select wraps");
    check(page.key(ENAGA_KEY_SELECT_LEFT, true) == Action::moved && page.group() == 1, "params: left wraps back");

    g_drawn.clear();
    enaga::Canvas canvas(g_pixels);
    page.draw(canvas);
    check(row(0) == "SYNTH|2/2", "params: title and group");
    check(row(12) == "A|CUTOFF|64", "params: row A");
    check(row(25) == "B|RESONANCE|3", "params: row B");
    check(row(38) == "", "params: no row C in a short group");
    const Drawn *changed = find("3"), *initial = find("64");
    check(changed && initial && changed->grey == 15 && initial->grey == 11, "params: changed values brighter");
    check(initial && initial->x == ENAGA_SCREEN_W - 12, "params: values right-aligned");
}

static void test_dialog() {
    Dialog dialog;
    check(!dialog.active() && dialog.key(ENAGA_KEY_OK, true) == Action::none, "dialog: closed takes nothing");
    dialog.open("SAVE OVER\nPATCH 3?");
    check(dialog.active(), "dialog: open");
    check(dialog.key(ENAGA_KEY_STEP1, true) == Action::moved && dialog.active(), "dialog: other keys swallowed");
    g_drawn.clear();
    enaga::Canvas canvas(g_pixels);
    dialog.draw(canvas);
    check(find("SAVE OVER") && find("PATCH 3?") && find("OK:YES  CLR:NO"), "dialog: lines and answers");
    check(dialog.key(ENAGA_KEY_OK, true) == Action::yes && !dialog.active(), "dialog: OK is yes and closes");
    dialog.open("DELETE?");
    check(dialog.key(ENAGA_KEY_CLR, true) == Action::no && !dialog.active(), "dialog: CLR is no and closes");
}

static void test_text_entry() {
    TextEntry entry;
    entry.open("NAME", "Bass_1", 8);
    check(std::strcmp(entry.text(), "BASS 1") == 0, "text: upper case; unknown characters become spaces");
    check(entry.encoder(1, 1) == Action::changed && entry.text()[0] == 'C', "text: knob B changes the character");
    check(entry.key(ENAGA_KEY_DOWN, true) == Action::changed && entry.text()[0] == 'B', "text: down steps back");
    check(entry.key(ENAGA_KEY_LEFT, true) == Action::none, "text: left stops at the start");
    check(entry.encoder(0, 20) == Action::moved && entry.cursor() == 7, "text: knob A moves, stopping at the end");
    entry.encoder(4, -1);                        // the last box: space back to '!'
    check(std::strcmp(entry.text(), "BASS 1 !") == 0, "text: VALUE wraps through the set");
    entry.encoder(4, 1);
    check(std::strcmp(entry.text(), "BASS 1") == 0, "text: trailing spaces trimmed");
    check(entry.key(ENAGA_KEY_F2, true) == Action::moved && entry.active(), "text: other keys swallowed");
    g_drawn.clear();
    enaga::Canvas canvas(g_pixels);
    entry.draw(canvas);
    check(row(0) == "NAME" && find("OK: DONE  CLR: CANCEL"), "text: title and controls");
    check(entry.key(ENAGA_KEY_OK, true) == Action::chosen && !entry.active(), "text: OK accepts and closes");
    check(std::strcmp(entry.text(), "BASS 1") == 0, "text: the answer stays readable");
    entry.open("NAME", nullptr, 40);
    check(std::strcmp(entry.text(), "") == 0, "text: no initial text");
    check(entry.encoder(0, 99) == Action::moved && entry.cursor() == TextEntry::max_chars - 1, "text: at most 16");
    check(entry.key(ENAGA_KEY_CLR, true) == Action::no && !entry.active(), "text: CLR cancels");
}

// The pixel at (x, y) of the last drawing.
int px(int x, int y) { return g_pixels[y * ENAGA_SCREEN_W + x]; }
void blank() { std::memset(g_pixels, 0, sizeof g_pixels); g_drawn.clear(); }
bool same(const char *a, const char *b) { return std::strcmp(a, b) == 0; }

static void test_line() {
    check(same(Line().add("TEMPO ").fixed(1300, 1).add(" BPM"), "TEMPO 130.0 BPM"), "line: chained parts");
    check(same(Line().number(-42), "-42") && same(Line().number(0), "0"), "line: numbers");
    check(same(Line().number(7, 3, '0'), "007") && same(Line().number(7, 3), "  7"), "line: width and padding");
    check(same(Line().number(-7, 3, '0'), "-07") && same(Line().number(-7, 4), "  -7"), "line: negative padded");
    check(same(Line().number(12345, 3), "12345"), "line: a wider number is not cut to the width");
    check(same(Line().fixed(-5, 2), "-0.05") && same(Line().fixed(1005, 1), "100.5"), "line: fixed point");
    check(same(Line().fixed(3, 0), "3"), "line: fixed point with no decimals");
    check(same(Line().hex(0x3C, 2).add(' ').hex(0xBEEF, 4), "3C BEEF"), "line: hex");
    check(same(Line().note(60), "C4") && same(Line().note(61), "C#4") && same(Line().note(0), "C-1")
          && same(Line().note(127), "G9") && same(Line().note(69), "A4"), "line: note names");
    check(same(Line().shown("kick_01.wav"), "KICK-01.WAV"), "line: outside text made drawable");
    check(same(Line().shown("abcdef", 3), "ABC"), "line: outside text cut at max");
    Line long_line("ABCDEFGHIJKLMNOPQRS");                 // 19 of 21
    check(!long_line.add_if_fits(" XYZ") && long_line.length() == 19, "line: a part that does not fit is left out");
    check(long_line.add_if_fits(" X") && long_line.room() == 0, "line: one that fits goes in");
    check(same(long_line.add("MORE"), "ABCDEFGHIJKLMNOPQRS X"), "line: cut at the end of the screen's line");
    check(same(Line("A").column(4).add("B"), "A   B"), "line: column");
    char small[5];
    Line("CUTOFF").copy_to(small, sizeof small);
    check(same(small, "CUTO"), "line: copy_to cuts to the buffer");
    for (const char *p = "AZ09 !#+-./:=?%>"; *p; p++) check(drawable(*p), "drawable: the font's characters");
    check(!drawable('a') && !drawable('_') && !drawable(','), "drawable: others are not");
}

static void test_frame_and_notice() {
    blank();
    enaga::Canvas canvas(g_pixels);
    status_line(canvas, "SONG 1", "120.0");
    hint_line(canvas, "OK: LOAD  CLR: BACK");
    hint_line(canvas, "UP DOWN: MOVE", 1);
    check(row(0) == "SONG 1|120.0" && find("120.0")->x == ENAGA_SCREEN_W - 30, "frame: status line");
    check(px(0, 9) == 3 && px(127, 9) == 3, "frame: a rule under the status line");
    check(row(hint_y) == "OK: LOAD  CLR: BACK" && row(hint_y - 10) == "UP DOWN: MOVE", "frame: hint lines");
    check(body_top > 9 && body_bottom < hint_y - 1, "frame: the body between them");

    Notice notice;
    blank();
    notice.draw(canvas);
    check(!notice.showing() && g_drawn.empty(), "notice: nothing before show");
    notice.show("SAVED");
    g_ms += 1000;
    notice.draw(canvas);
    check(notice.showing() && row(hint_y) == "SAVED" && px(0, hint_y - 2) == 4, "notice: shown on a bar");
    g_ms += 600;
    check(!notice.showing() && same(notice.text(), ""), "notice: gone after 1.5 s");
    char text[] = "LOADED";
    notice.show(text, 100);
    text[0] = 'X';
    check(same(notice.text(), "LOADED"), "notice: the text is copied");
    notice.clear();
    check(!notice.showing(), "notice: clear");
    g_ms = 0xFFFFFF00u;                                  // shown across the millisecond counter's wrap
    notice.show("WRAP");
    g_ms += 0x200;
    check(notice.showing(), "notice: across the counter's wrap");
}

static void test_bars_and_meters() {
    enaga::Canvas canvas(g_pixels);
    blank();
    bar(canvas, 10, 50, 40, 64, 0, 127);
    check(px(10, 50) == 9 && px(29, 50) == 9 && px(30, 50) == 2 && px(49, 50) == 2 && px(50, 50) == 0, "bar: lit to the value");
    blank();
    bar(canvas, 10, 50, 40, 500, 0, 127, 3);
    check(px(49, 52) == 9 && px(50, 52) == 0, "bar: clamped, and h high");
    blank();
    bar_centred(canvas, 0, 50, 40, -25, -50, 50);
    check(px(10, 50) == 9 && px(9, 50) == 2 && px(19, 50) == 9 && px(20, 49) == 6 && px(25, 50) == 2,
          "bar_centred: from the middle to the left");
    blank();
    bar_centred(canvas, 0, 50, 40, 50, -50, 50);
    check(px(21, 50) == 9 && px(39, 50) == 9 && px(15, 50) == 2, "bar_centred: and to the right");
    blank();
    meter(canvas, 0, 20, 4, 40, 0.5f, 0.75f);                // upright: bottom half lit, the peak above it
    check(px(0, 59) == 9 && px(0, 40) == 9 && px(0, 39) == 2 && px(3, 30) == 15 && px(0, 31) == 2, "meter: upright");
    blank();
    meter(canvas, 0, 20, 40, 3, 1.5f, 1.0f);                 // across, over full scale: all lit, peak at the end
    check(px(38, 21) == 9 && px(39, 21) == 15, "meter: across, clamped");
    blank();
    meter(canvas, 0, 20, 40, 3, 0.0f);
    check(px(0, 20) == 2 && px(39, 22) == 2, "meter: empty with no peak");
    auto near = [](float a, float b) { return a - b < 0.01f && b - a < 0.01f; };
    check(near(meter_db(1.0f), 1.0f) && near(meter_db(-1.0f), 1.0f), "meter_db: full scale");
    check(near(meter_db(0.5f), 1.0f - 6.0206f / 48.0f), "meter_db: -6 dB");
    check(meter_db(0.0f) == 0.0f && meter_db(0.001f) == 0.0f && near(meter_db(0.1f, -40.0f), 0.5f), "meter_db: floor");
    check(meter_db(2.0f) == 1.0f, "meter_db: over full scale");
}

int main() {
    test_line();
    test_frame_and_notice();
    test_bars_and_meters();
    test_list();
    test_param_page();
    test_dialog();
    test_text_entry();
    std::printf(failures ? "%d failed\n" : "ok\n", failures);
    return failures ? 1 : 0;
}
