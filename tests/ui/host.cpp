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

// The file service's listing, by hand: each request is kept, and the test delivers its entries and its end.
struct Listing { std::string path; enaga_file_entry_t entry; enaga_file_done_t done; void *user; };
std::vector<Listing> g_listings;
int32_t g_list_result = ENAGA_FILE_OK;
int32_t host_file_list(const char *path, enaga_file_entry_t entry, enaga_file_done_t done, void *user) {
    if (g_list_result < 0) return g_list_result;
    g_listings.push_back({path, entry, done, user});
    return ENAGA_FILE_OK;
}
void deliver(size_t request, std::vector<std::pair<const char *, bool>> entries, int32_t result = -1) {
    Listing &l = g_listings[request];
    for (auto &[name, folder] : entries) {
        enaga_file_info_t info{};
        std::snprintf(info.name, sizeof info.name, "%s", name);
        info.flags = folder ? ENAGA_FILE_FOLDER : 0;
        l.entry(l.user, &info);
    }
    l.done(l.user, result < 0 && result != -1 ? result : int32_t(entries.size()));
}

enaga_host_t g_host = [] {
    enaga_host_t h{};
    h.text = host_text; h.ms = host_ms; h.file_list = host_file_list;
    return h;
}();
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
    check(same(Line().shown("kick_01.wav"), "KICK_01.WAV"), "line: outside text made drawable");
    check(same(Line().shown("a\tb~c\xC3\xA9"), "A-B-C--"), "line: what the font lacks shown as '-'");
    check(same(Line().shown("abcdef", 3), "ABC"), "line: outside text cut at max");
    Line long_line("ABCDEFGHIJKLMNOPQRS");                 // 19 of 21
    check(!long_line.add_if_fits(" XYZ") && long_line.length() == 19, "line: a part that does not fit is left out");
    check(long_line.add_if_fits(" X") && long_line.room() == 0, "line: one that fits goes in");
    check(same(long_line.add("MORE"), "ABCDEFGHIJKLMNOPQRS X"), "line: cut at the end of the screen's line");
    check(same(Line("A").column(4).add("B"), "A   B"), "line: column");
    char small[5];
    Line("CUTOFF").copy_to(small, sizeof small);
    check(same(small, "CUTO"), "line: copy_to cuts to the buffer");
    for (const char *p = "AZaz09 !#+-./:=?%><(),_'*[]"; *p; p++) check(drawable(*p), "drawable: the font's characters");
    check(!drawable('~') && !drawable('"') && !drawable(';') && !drawable('\xC3'), "drawable: others are not");
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

static void test_grid() {
    static const char *const names[] = {"A1", "A2", "A3", "A4", "B1", "B2", "B3", "B4", "C1", "C2"};
    Grid grid;
    grid.set_title("BANK");
    grid.set_items(names, 10, 4);
    check(grid.key(ENAGA_KEY_LEFT, true) == Action::none && grid.key(ENAGA_KEY_UP, true) == Action::none,
          "grid: stops at the top left");
    check(grid.key(ENAGA_KEY_RIGHT, true) == Action::moved && grid.cursor() == 1, "grid: right");
    check(grid.key(ENAGA_KEY_DOWN, true) == Action::moved && grid.cursor() == 5, "grid: down a row");
    check(grid.key(ENAGA_KEY_DOWN, true) == Action::moved && grid.cursor() == 9, "grid: down to the short last row");
    check(grid.key(ENAGA_KEY_RIGHT, true) == Action::none, "grid: no cell to the right in the last row");
    grid.select(3);
    check(grid.key(ENAGA_KEY_RIGHT, true) == Action::none, "grid: right stops at the row's end, no wrap");
    check(grid.key(ENAGA_KEY_DOWN, true) == Action::moved && grid.cursor() == 7, "grid: down from the corner");
    check(grid.key(ENAGA_KEY_DOWN, true) == Action::none, "grid: nothing under it");
    check(grid.encoder(4, 1) == Action::moved && grid.cursor() == 8, "grid: VALUE goes on to the next row");
    check(grid.encoder(0, 1) == Action::none, "grid: knobs are not the grid's");
    check(grid.key(ENAGA_KEY_OK, true) == Action::chosen && grid.key(ENAGA_KEY_CLR, true) == Action::back,
          "grid: OK and CLR");
    grid.light(2, true);
    grid.light(70, true);
    check(grid.lit(2) && !grid.lit(3) && !grid.lit(70), "grid: lights, and none past max_cells");
    blank();
    enaga::Canvas canvas(g_pixels);
    grid.select(1);
    grid.draw(canvas);
    check(row(0) == "BANK" && row(14) == "A1|A2|A3|A4", "grid: title and a row of cells");
    const Drawn *a3 = find("A3"), *a2 = find("A2"), *a1 = find("A1");
    check(a3 && a3->grey == 0 && px(66, 13) == 9, "grid: a lit cell is filled, its label dark");
    check(a2 && a2->grey == 15 && px(32, 11) == 15, "grid: the cursor's cell framed and bright");
    check(a1 && a1->grey == 10 && px(2, 13) == 2 && a2->x == 34, "grid: others dim; cells a quarter wide");
    grid.clear_lights();
    check(!grid.lit(2), "grid: clear_lights");

    Grid many;                                            // scrolling: 16 rows of 4 in 3 visible rows
    many.set_items(64, 4, [](int i, char *out, int size, void *) { std::snprintf(out, size, "%02d", i); }, nullptr);
    many.select(63);
    blank();
    many.draw(canvas, 0, 36);
    check(find("63") && find("55") && !find("51"), "grid: scrolled to the cursor's row");
    Grid narrow;
    narrow.set_items(names, 3, 8);
    blank();
    narrow.draw(canvas);
    check(find("A1") && find("A1")->text == "A1" && px(16, 2) == 0, "grid: a narrow cell shows what fits");
}

static void test_pad_map() {
    PadMap pads;
    check(PadMap::x(0) == 0 && PadMap::x(14) == 112 && PadMap::x(1) == 8 && PadMap::x(13) == 104,
          "pads: lower and upper rows staggered");
    check(!PadMap::upper(0) && PadMap::upper(1) && !PadMap::upper(14), "pads: odd pads above");
    pads.label(0, "C");
    pads.label(1, "C#X");
    pads.label(99, "NO");
    pads.light(1, 12);
    pads.light(2, 200);
    pads.select(2);
    check(pads.selected() == 2, "pads: select");
    blank();
    enaga::Canvas canvas(g_pixels);
    pads.draw(canvas, 40);
    const Drawn *c = find("C"), *cs = find("C#");
    check(c && c->x == 2 && c->y == 40 + 14 + 4 && c->grey == 15, "pads: a dark lower pad's label");
    check(cs && cs->x == 10 && cs->y == 44 && cs->grey == 0, "pads: a lit upper pad, label cut to two");
    check(px(9, 41) == 12 && px(1, 55) == 2, "pads: their light");
    check(px(16, 54) == 15 && px(31, 67) == 15 && px(32, 54) == 0, "pads: the selected pad framed");
    check(px(17, 55) == 15, "pads: light clamped at 15");
    pads.select(-3);
    check(pads.selected() == -1, "pads: none selected");
}

static void test_param_tabs_and_columns() {
    static const Param params[] = {
        {"CUTOFF", 0, 127, 64}, {"RESONANCE", 0, 100, 0}, {"DRIVE", 0, 10, 0}, {"MODE", 0, 2, 0},
        {"ATTACK", 0, 2000, 10, nullptr, nullptr, "MS"},
    };
    static const char *const names[] = {"FLT", "ENV"};
    int values[5] = {64, 0, 0, 0, 10};
    ParamPage page("SYNTH", params, 5, values);
    page.set_group_names(names);
    blank();
    enaga::Canvas canvas(g_pixels);
    page.draw(canvas);
    check(row(0) == "SYNTH|FLT|ENV", "tabs: the group names on the title line");
    check(find("FLT")->grey == 15 && find("ENV")->grey == 6 && find("ENV")->x == ENAGA_SCREEN_W - 18,
          "tabs: the current one lit, right-aligned");
    ParamPage long_title("A LONG PAGE TITLE", params, 5, values);
    long_title.set_group_names(names);
    blank();
    long_title.draw(canvas);
    check(row(0) == "A LONG PAGE TITLE|1/2", "tabs: 1/2 when the names do not fit");

    page.set_columns(true);
    page.encoder(0, 5);
    blank();
    page.draw(canvas);
    check(row(14) == "A|B|C|D", "columns: the knob letters across");
    check(row(25) == "CUTOF|RESON|DRIVE|MODE", "columns: names, five characters");
    check(row(38) == "69|0|0|0" && find("69")->x == 0 && find("69")->grey == 15, "columns: values under them");
    check(px(0, 48) == 9 && px(27, 48) == 2 && px(32, 48) == 2, "columns: bars under each");
    page.key(ENAGA_KEY_SELECT_RIGHT, true);
    blank();
    page.draw(canvas);
    check(row(25) == "ATTAC" && row(38) == "10MS", "columns: a short last group");
}

static void test_file_browser() {
    g_listings.clear();
    static FileBrowser browser;                           // large: not on the stack
    browser.open("/SAMPLES", ".wav");
    check(g_listings.size() == 1 && g_listings[0].path == "/SAMPLES" && browser.loading(), "files: listing asked for");
    blank();
    enaga::Canvas canvas(g_pixels);
    browser.draw(canvas);
    check(row(0) == "/SAMPLES" && find("LOADING"), "files: loading");
    check(browser.key(ENAGA_KEY_OK, true) == Action::none, "files: OK does nothing while loading");
    deliver(0, {{"snare.WAV", false}, {"Kicks", true}, {"notes.txt", false}, {"bass.wav", false}, {"Amb", true}});
    check(!browser.loading() && browser.count() == 4, "files: the listing, other files left out");
    check(std::string(browser.name(0)) == "Amb" && browser.is_folder(1) && std::string(browser.name(2)) == "bass.wav"
          && std::string(browser.name(3)) == "snare.WAV", "files: folders first, then by name in any case");
    blank();
    browser.draw(canvas);
    check(find("AMB/") && find("KICKS/") && find("BASS.WAV"), "files: shown upper case, folders with '/'");
    browser.key(ENAGA_KEY_DOWN, true);
    browser.key(ENAGA_KEY_DOWN, true);
    check(browser.key(ENAGA_KEY_OK, true) == Action::chosen && std::string(browser.path()) == "/SAMPLES/bass.wav",
          "files: OK on a file chooses it, with its path");
    browser.key(ENAGA_KEY_UP, true);
    check(browser.key(ENAGA_KEY_OK, true) == Action::moved && std::string(browser.folder()) == "/SAMPLES/Kicks"
          && g_listings.size() == 2 && browser.loading(), "files: OK on a folder goes in");
    deliver(1, {});
    blank();
    browser.draw(canvas);
    check(row(0) == "/SAMPLES/KICKS" && find("EMPTY"), "files: an empty folder");
    check(browser.key(ENAGA_KEY_CLR, true) == Action::moved && std::string(browser.folder()) == "/SAMPLES",
          "files: CLR goes back up");
    check(browser.key(ENAGA_KEY_CLR, true) == Action::back, "files: CLR at the start gives back");
    // Moving on while a listing runs: its entries are not the new folder's.
    deliver(2, {{"old.wav", false}});
    browser.open("/SAMPLES", ".wav");                     // a fresh listing
    browser.key(ENAGA_KEY_DOWN, true);
    browser.key(ENAGA_KEY_OK, true);                      // nothing yet: OK does nothing
    browser.open("/RECORD");
    check(g_listings.size() == 5, "files: two listings asked for");
    deliver(3, {{"stale.wav", false}});                   // the /SAMPLES one, finishing late
    check(browser.loading() && browser.count() == 0, "files: a late listing is not shown");
    deliver(4, {{"take 1.wav", false}, {"x.raw", false}});
    check(!browser.loading() && browser.count() == 2 && std::string(browser.name(0)) == "take 1.wav",
          "files: the new folder's, with no extension filter");
    g_list_result = ENAGA_FILE_NO_CARD;
    browser.open("/SAMPLES");
    blank();
    browser.draw(canvas);
    check(browser.error() == ENAGA_FILE_NO_CARD && find("NO CARD") && !browser.loading(), "files: an error shown");
    g_list_result = ENAGA_FILE_OK;
    browser.open("/");
    deliver(5, {{"SAMPLES", true}}, ENAGA_FILE_CARD_ERROR);
    check(browser.error() == ENAGA_FILE_CARD_ERROR, "files: an error at the end of a listing");
    std::string longname(70, 'a');
    browser.open("/");
    deliver(6, {{longname.c_str(), false}, {"ok", false}});
    check(browser.count() == 1, "files: a name too long to keep is left out");
}

static void test_param_steps_and_speed() {
    static const Param params[] = {
        {.name = "TEMPO", .min = 20, .max = 300, .initial = 120, .unit = "BPM"},
        {.name = "DELAY", .min = 0, .max = 2000, .initial = 250, .unit = "MS", .step = 10},
        {.name = "MIX", .min = 0, .max = 100, .initial = 50},
    };
    int values[3] = {120, 250, 50};
    ParamPage page("FX", params, 3, values);
    g_ms = 10000;
    page.encoder(1, 1);
    check(values[1] == 260, "speed: a step of 10 a detent");
    g_ms += 200;
    page.encoder(0, 1);
    check(values[0] == 121, "speed: one detent, turned slowly");
    g_ms += 100; page.encoder(0, 1);
    check(values[0] == 122, "speed: 100 ms later, still one");
    g_ms += 40; page.encoder(0, 1);
    check(values[0] == 125, "speed: 40 ms later, three");
    g_ms += 10; page.encoder(0, 1);
    check(values[0] == 133, "speed: 10 ms later, eight");
    g_ms += 10; page.encoder(0, -1);
    check(values[0] == 132, "speed: turning back starts again at one");
    g_ms += 10; page.encoder(1, -1);
    check(values[1] == 250, "speed: another knob starts at one");
    g_ms += 10; page.encoder(1, -1);
    check(values[1] == 170, "speed: then eight steps of 10 (2000 / 10 = 200 steps: wide)");
    g_ms += 10; page.encoder(2, 1);
    g_ms += 10; page.encoder(2, 1);
    check(values[2] == 52, "speed: a range of 100 is not wide: never faster");
    page.set_acceleration(false);
    g_ms += 10; page.encoder(0, 1);
    g_ms += 10; page.encoder(0, 1);
    check(values[0] == 134, "speed: off");
    page.encoder(0, 1000);
    check(values[0] == 300, "speed: clamped");
    page.set_acceleration(true);
    page.key(ENAGA_KEY_PUSH_B, true);
    check(values[1] == 250, "speed: a push still resets");

    static const Param many[] = {{"A", 0, 9, 0}, {"B", 0, 9, 0}, {"C", 0, 9, 0}, {"D", 0, 9, 0}, {"E", 0, 9, 0}};
    int first[5] = {}, second[5] = {1, 2, 3, 4, 5};
    ParamPage tracks("TRACK", many, 5, first);
    tracks.key(ENAGA_KEY_SELECT_RIGHT, true);
    tracks.set_values(second);
    check(tracks.group() == 1 && tracks.value(4) == 5, "set_values: other values, the same group");
    tracks.encoder(0, 1);
    check(second[4] == 6 && first[4] == 0, "set_values: turns change the new array");
}

int main() {
    test_line();
    test_param_steps_and_speed();
    test_grid();
    test_pad_map();
    test_param_tabs_and_columns();
    test_file_browser();
    test_frame_and_notice();
    test_bars_and_meters();
    test_list();
    test_param_page();
    test_dialog();
    test_text_entry();
    std::printf(failures ? "%d failed\n" : "ok\n", failures);
    return failures ? 1 : 0;
}
