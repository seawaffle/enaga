// Enaga's host table for tests on a PC (tools/host_test.py builds it in; app builds never do): what an app's code
// reaches through enaga::host() when it runs without Enaga. <enaga/testing.hpp> declares the controls.
//
// What it does: log prints the line (and keeps it, testing::logged()); ms is a clock the test sets; fatal prints
// the reason and ends the run with exit status 3; midi_send keeps what was sent (testing::midi_sent()); the clock
// runs at a tempo the test sets, each clock_ticks call taking the next block's ticks; the file and stream services
// answer as if no card were in (ENAGA_FILE_NO_CARD); the panel's lights, the speaker, the input and the menu do
// nothing. The text drawing does nothing either: draw() can run, and the canvas's pixels are there to read,
// but text leaves none.
#include <enaga/app.hpp>
#include <enaga/testing.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

std::vector<std::string> g_logged;
std::vector<std::vector<uint8_t>> g_midi;
uint32_t g_ms = 0;
enaga_clock_state_t g_clock = {0, 0, 120.0f, 0, 0.0f};
double g_position = 0.0;                   // ticks, with the fraction

constexpr int32_t no_card = ENAGA_FILE_NO_CARD;

uint32_t clock_ticks(enaga_clock_tick_t *ticks, uint32_t max) {
    if (!g_clock.running) return 0;
    const double per_frame = g_clock.tempo * double(ENAGA_CLOCK_PPQN) / 60.0 / ENAGA_SAMPLE_RATE;
    const double start = g_position;
    g_position += per_frame * ENAGA_BLOCK_FRAMES;
    uint32_t n = 0;
    for (uint64_t t = uint64_t(start) + (start > double(uint64_t(start)) ? 1 : 0); double(t) < g_position && n < max;
         t++) {
        ticks[n].tick = uint32_t(t);
        ticks[n].frame = uint32_t((double(t) - start) / per_frame);
        if (ticks[n].frame >= ENAGA_BLOCK_FRAMES) ticks[n].frame = ENAGA_BLOCK_FRAMES - 1;
        n++;
    }
    g_clock.position = uint32_t(g_position);
    g_clock.fraction = float(g_position - double(uint64_t(g_position)));
    return n;
}

enaga_host_t make() {
    enaga_host_t h{};
    h.abi_version = ENAGA_ABI_VERSION;
    h.size = sizeof h;
    h.log = [](const char *text) {
        g_logged.emplace_back(text);
        std::printf("log: %s\n", text);
    };
    h.ms = [] { return g_ms; };
    h.fatal = [](const char *reason) {
        std::printf("fatal: %s\n", reason);
        std::fflush(stdout);
        std::exit(3);
    };
    h.sample_rate = ENAGA_SAMPLE_RATE;
    h.block_frames = ENAGA_BLOCK_FRAMES;
    h.text = [](uint8_t *, int, int, const char *, uint8_t) {};
    h.audio_load = [] { return 0u; };
    h.audio_load_peak = [] { return 0u; };
    h.led = [](uint32_t, uint32_t, uint32_t, uint32_t) {};
    h.file_open = [](const char *, uint32_t) { return no_card; };
    h.file_close = [](int32_t) { return no_card; };
    h.file_size = [](int32_t) { return no_card; };
    h.file_seek = [](int32_t, uint32_t) { return no_card; };
    h.file_info = [](const char *, enaga_file_info_t *) { return no_card; };
    h.file_remove = [](const char *) { return no_card; };
    h.file_make_folder = [](const char *) { return no_card; };
    h.file_read_all = [](const char *, void *, uint32_t) { return no_card; };
    h.file_save = [](const char *, const void *, uint32_t) { return no_card; };
    h.file_read = [](int32_t, void *, uint32_t, enaga_file_done_t, void *) { return no_card; };
    h.file_write = [](int32_t, const void *, uint32_t, enaga_file_done_t, void *) { return no_card; };
    h.file_load = [](const char *, void *, uint32_t, enaga_file_done_t, void *) { return no_card; };
    h.file_list = [](const char *, enaga_file_entry_t, enaga_file_done_t, void *) { return no_card; };
    h.file_data_folder = [] { return "/ENAGA/DATA/TEST"; };
    h.stream_open = [](const char *, uint32_t, void *, uint32_t) { return no_card; };
    h.stream_create = [](const char *, uint32_t, void *, uint32_t) { return no_card; };
    h.stream_read = [](int32_t, void *, uint32_t) { return 0u; };
    h.stream_write = [](int32_t, const void *, uint32_t) { return 0u; };
    h.stream_status = [](int32_t, enaga_stream_status_t *) { return no_card; };
    h.stream_close = [](int32_t, const void *, uint32_t) { return no_card; };
    h.open_menu = [] {};
    h.set_busy = [](uint32_t) {};
    h.speaker = [](uint32_t) {};
    h.headphones = [] { return int32_t(0); };
    h.device = [] { return uint32_t(ENAGA_DEVICE_SMPLTREK); };
    h.input = [](uint32_t, uint32_t) { return int32_t(0); };
    h.midi_send = [](uint32_t, const uint8_t *data, uint32_t bytes) {
        g_midi.emplace_back(data, data + bytes);
        return int32_t(bytes);
    };
    h.midi_channel = [] { return 0u; };
    h.clock_control = [](uint32_t, int32_t) { return int32_t(0); };
    h.clock_state = [](enaga_clock_state_t *state) { *state = g_clock; };
    h.clock_ticks = clock_ticks;
    h.battery = [] { return int32_t(-1); };
    return h;
}

const enaga_host_t g_host = make();

}  // namespace

namespace enaga {
const enaga_host_t &host() { return g_host; }
[[noreturn]] void fatal(const char *reason) {
    g_host.fatal(reason);
    std::exit(3);
}

namespace testing {
const std::vector<std::string> &logged() { return g_logged; }
const std::vector<std::vector<uint8_t>> &midi_sent() { return g_midi; }
void clear_host() {
    g_logged.clear();
    g_midi.clear();
}
void set_ms(uint32_t ms) { g_ms = ms; }
void run_clock(float bpm, uint32_t from_tick) {
    g_clock.running = 1;
    g_clock.tempo = bpm;
    g_position = from_tick;
    g_clock.position = from_tick;
    g_clock.fraction = 0.0f;
}
void stop_clock() { g_clock.running = 0; }
}  // namespace testing
}  // namespace enaga
