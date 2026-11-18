// CHECK: the app for sessions on a unit (docs/hardware.md "Checks on a unit"). Four pages; hold F4 for one
// second to go to the next one (the press still counts on the panel page).
//
//   AUDIO  STEP 1: 750 Hz left only, STEP 2: 1500 Hz right only, STEP 3: 750 Hz both, STEP 4: off. VALUE: level.
//          IN L/R: input peaks over the last half second, per mille of full scale.
//   LOAD   STEP 1: SYNTH voices (saw + biquad), STEP 2: SAMPLE voices (16-bit playback streaming through an
//          8 MiB bank in DDR), STEP 3: GRAIN voices (as SAMPLE, but each voice jumps to a random place every
//          block). STEP 5-8: 16, 64, 128, 256 voices; VALUE: voices +-1, A: +-8. Shows the highest load in the
//          last second (percent of the block budget), cycles per voice (the load / voices, Enaga's own overhead
//          included), and the voices that would fit in 90 % of the budget at that rate. A block over 90 % cuts the voices to fit 85 % (CAP):
//          past 100 % the callback would take every cycle and the main loop, keys included, would stop.
//   PANEL  the last key by name and bit with its press count (one press must count one: bounce), every encoder's
//          position, the pad with the highest raw reading, and pad messages per second (the panel MCU sends
//          every pad all the time, about 3,930 a second for the 15 pads). A held key lights its LED green; a pad lights green by
//          pressure; A, B and C set VOL L, VOL R and MIC (0-10); VALUE cycles the VALUE LED red, green, blue.
//   CARD   STEP 1 measures the card through Enaga's file service, as an app gets it (queued in the main loop, 2 ms
//          a pass): WRITE, a 4 MiB file to /RECORD/CHECK.BIN in 32 KiB writes, four queued at a time; READ, the
//          file back the same way; SCATTER, 256 reads of 4 KiB at random places in it, one after another (seek,
//          read). KiB/s for each, SCATTER's reads a second, and how many 48 kHz 16-bit stereo streams (187.5
//          KiB/s each) READ's rate would feed. The file is removed afterwards; the results also go to the log.
#include <enaga/app.hpp>
#include <enaga/files.hpp>

#include <cstdint>

namespace {

constexpr int kTable = 64;
constexpr uint32_t kBlockCycles = 496000;          // the 64-frame budget at 372 MHz; load is per mille of it
constexpr uint32_t kBankSamples = 4u << 20;        // 8 MiB of 16-bit samples
constexpr int kMaxVoices = 512;
constexpr int kPageCount = 4;

constexpr const char *kCardPath = "/RECORD/CHECK.BIN";
constexpr uint32_t kCardBytes = 4u << 20;           // the test file
constexpr uint32_t kChunk = 32 * 1024;              // WRITE and READ request size
constexpr uint32_t kInFlight = 4;                   // requests queued at a time
constexpr uint32_t kScatterBytes = 4096, kScatterReads = 256;

const char *const kKeyNames[ENAGA_KEY_COUNT] = {
    "PUSH B", "PUSH A", "PUSH C", "PUSH D", "PUSH VALUE", nullptr, nullptr, nullptr,
    "SELECT L", "SELECT R", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    "OCTAVE L", "UP", "RIGHT", "F1", "F3", "F7", "F5", "OK",
    "LEFT", "DOWN", "F2", "F4", "F8", "F6", "CLR", "OCTAVE R",
    "STEP 1", "STEP 2", "STEP 3", "STEP 4", "STEP 5", "STEP 6", "STEP 7", "STEP 8",
    "STEP 9", "STEP 10", "STEP 11", "STEP 12", "STEP 13", "STEP 14", "STEP 15", "STEP 16",
};

class Line {
public:
    Line &operator<<(const char *t) { while (*t && n_ < 31) s_[n_++] = *t++; s_[n_] = 0; return *this; }
    Line &operator<<(uint32_t v) {
        char d[12];
        int k = 0;
        do { d[k++] = char('0' + v % 10); v /= 10; } while (v);
        while (k && n_ < 31) s_[n_++] = d[--k];
        s_[n_] = 0;
        return *this;
    }
    Line &operator<<(int v) { if (v < 0) { *this << "-"; v = -v; } return *this << uint32_t(v); }
    const char *str() const { return s_; }

private:
    char s_[32] = {};
    int n_ = 0;
};

uint32_t clamp(int v, int lo, int hi) { return uint32_t(v < lo ? lo : v > hi ? hi : v); }

struct Synth { float phase, inc, z1, z2; };
struct Sample { uint32_t pos, frac, inc; };

class Check : public enaga::App {
public:
    bool init() override {
        const float c = 0.99518472667f, s = 0.09801714033f;   // cos/sin of 2 pi / 64
        float x = 1.0f, y = 0.0f;
        for (int i = 0; i < kTable; i++) {
            table_[i] = y;
            float nx = x * c - y * s;
            y = x * s + y * c;
            x = nx;
        }
        table_[kTable] = table_[0];
        bank_ = new int16_t[kBankSamples + 1];
        card_buffer_ = new uint8_t[kChunk];
        uint32_t r = 1;
        for (uint32_t i = 0; i <= kBankSamples; i++) { r = r * 1664525u + 1013904223u; bank_[i] = int16_t(r >> 16); }
        for (int v = 0; v < kMaxVoices; v++) {
            synth_[v] = {0.0f, (55.0f + 3.0f * float(v)) / 48000.0f, 0.0f, 0.0f};
            sample_[v] = {uint32_t(v) * (kBankSamples / kMaxVoices), 0, 65536u + uint32_t(v) * 65u};
        }
        enaga::log("check ready");
        return true;
    }

    void update(uint32_t ms) override {
        if (home_down_ && !home_used_ && ms - home_since_ >= 1000) {
            home_used_ = true;
            leds_off();
            page_ = (page_ + 1) % kPageCount;
        }
        uint32_t load = enaga::audio_load();
        if (load > load_window_) load_window_ = load;
        if (ms - load_since_ >= 1000) {
            load_shown_ = load_window_;
            load_window_ = 0;
            load_since_ = ms;
            pads_per_second_ = pad_messages_ - pad_messages_last_;
            pad_messages_last_ = pad_messages_;
        }
        float l = in_peak_[0], r = in_peak_[1];
        if (l > in_window_[0]) in_window_[0] = l;
        if (r > in_window_[1]) in_window_[1] = r;
        if (ms - in_since_ >= 500) {
            in_shown_[0] = uint32_t(in_window_[0] * 1000.0f);
            in_shown_[1] = uint32_t(in_window_[1] * 1000.0f);
            in_window_[0] = in_window_[1] = 0.0f;
            in_since_ = ms;
        }
    }

    void on_key(int key, bool down) override {
        if (key == ENAGA_KEY_F4) {
            home_down_ = down;
            if (down) { home_since_ = enaga::ms(); home_used_ = false; }
        }
        if (key >= 0 && key < ENAGA_KEY_COUNT) {
            last_key_ = key;
            last_down_ = down;
            if (down) presses_[key]++;
            events_++;
        }
        if (page_ == 2) enaga::button_led(key, 0, down ? ENAGA_LED_MAX : 0);
        if (!down || key < ENAGA_KEY_STEP1 || key > ENAGA_KEY_STEP1 + 7) return;
        static const uint32_t kPresets[4] = {16, 64, 128, 256};
        int step = key - ENAGA_KEY_STEP1;
        if (page_ == 0 && step < 4) tone_ = uint32_t(step + 1) % 4;
        if (page_ == 1 && step < 3) mode_ = uint32_t(step);
        if (page_ == 1 && step >= 4) voices_ = kPresets[step - 4];
        if (page_ == 1) { load_window_ = 0; uncap_ = uncap_ + 1; }
        if (page_ == 3 && step == 0) card_start();
    }

    void on_encoder(int encoder, int delta) override {
        if (encoder >= 0 && encoder < 5) position_[encoder] += delta;
        if (page_ == 0 && encoder == 4) level_ = clamp(int(level_) + 5 * delta, 0, 100);
        if (page_ == 1 && (encoder == 4 || encoder == 1)) {
            voices_ = clamp(int(voices_) + delta * (encoder == 4 ? 1 : 8), 0, kMaxVoices);
            load_window_ = 0;
            uncap_ = uncap_ + 1;
        }
        if (page_ == 2 && encoder < 3)
            (encoder == 2 ? enaga::mic_led(clamp(position_[2], 0, 10))
                          : enaga::vol_led(encoder, clamp(position_[encoder], 0, 10)));
        if (page_ == 2 && encoder == 4) {
            uint32_t c = uint32_t(position_[4] % 3 + 3) % 3;
            enaga::value_led(c == 0 ? ENAGA_LED_MAX : 0, c == 1 ? ENAGA_LED_MAX : 0, c == 2 ? ENAGA_LED_MAX : 0);
        }
    }

    void on_pad(int pad, unsigned pressure) override {
        if (pad < 0 || pad >= 15) return;
        pad_raw_[pad] = pressure;
        pad_messages_++;
        if (page_ == 2) enaga::pad_led(pad, 0, pressure * ENAGA_LED_MAX / 16383u, 0);
    }

    void audio(const float *in, float *out, uint32_t frames) override {
        float peak[2] = {0.0f, 0.0f};
        for (uint32_t i = 0; i < 2 * frames; i++) {
            float a = in[i] < 0.0f ? -in[i] : in[i];
            if (a > peak[i & 1]) peak[i & 1] = a;
        }
        in_peak_[0] = peak[0];
        in_peak_[1] = peak[1];
        for (uint32_t i = 0; i < 2 * frames; i++) out[i] = 0.0f;
        if (page_ == 0) tone(out, frames);
        if (page_ == 1) load(out, frames);
    }

    void draw(enaga::Canvas &canvas) override {
        static const char *const kPages[kPageCount] = {"AUDIO", "LOAD", "PANEL", "CARD"};
        canvas.clear();
        canvas.text(0, 0, "CHECK");
        canvas.text(48, 0, kPages[page_], 10);
        canvas.text(0, 118, "HOLD F4: NEXT PAGE", 6);
        if (page_ == 0) draw_audio(canvas);
        if (page_ == 1) draw_load(canvas);
        if (page_ == 2) draw_panel(canvas);
        if (page_ == 3) draw_card(canvas);
    }

private:
    void tone(float *out, uint32_t frames) {
        const uint32_t which = tone_;
        if (!which) { phase_ = 0; return; }
        const uint32_t increment = (which == 2 ? 2u : 1u) * (uint32_t(kTable) << 16) / 64u;
        const float level = float(level_) * 0.01f;
        for (uint32_t i = 0; i < frames; i++) {
            uint32_t index = phase_ >> 16;
            float frac = float(phase_ & 0xFFFF) * (1.0f / 65536.0f);
            float sample = level * (table_[index] + (table_[index + 1] - table_[index]) * frac);
            phase_ = (phase_ + increment) & ((uint32_t(kTable) << 16) - 1);
            if (which != 2) out[2 * i] = sample;
            if (which != 1) out[2 * i + 1] = sample;
        }
    }

    void load(float *out, uint32_t frames) {
        float mix[ENAGA_BLOCK_FRAMES] = {};
        if (uncap_ != uncap_seen_) { uncap_seen_ = uncap_; cap_ = kMaxVoices; }
        const uint32_t wanted = voices_, mode = mode_, last = enaga::audio_load();   // the previous block
        uint32_t n = wanted < cap_ ? wanted : cap_;
        if (last > 900 && n > 1) cap_ = n = n * 850 / last;
        if (frames > ENAGA_BLOCK_FRAMES) frames = ENAGA_BLOCK_FRAMES;
        for (uint32_t v = 0; v < n; v++) {
            if (mode == 0) {
                Synth s = synth_[v];
                const float b0 = 0.0675f, b1 = 0.135f, b2 = 0.0675f, a1 = -1.143f, a2 = 0.413f;
                for (uint32_t i = 0; i < frames; i++) {
                    s.phase += s.inc;
                    if (s.phase >= 1.0f) s.phase -= 1.0f;
                    float x = 2.0f * s.phase - 1.0f;
                    float y = b0 * x + s.z1;
                    s.z1 = b1 * x - a1 * y + s.z2;
                    s.z2 = b2 * x - a2 * y;
                    mix[i] += y;
                }
                synth_[v] = s;
            } else {
                Sample s = sample_[v];
                if (mode == 2) { random_ = random_ * 1664525u + 1013904223u; s.pos = random_ >> 10; }
                for (uint32_t i = 0; i < frames; i++) {
                    float a = float(bank_[s.pos]), b = float(bank_[s.pos + 1]);
                    mix[i] += (a + (b - a) * float(s.frac) * (1.0f / 65536.0f)) * (1.0f / 32768.0f);
                    s.frac += s.inc;
                    s.pos = (s.pos + (s.frac >> 16)) & (kBankSamples - 1);
                    s.frac &= 0xFFFF;
                }
                sample_[v] = s;
            }
        }
        const float gain = 0.02f / float(n ? n : 1);
        for (uint32_t i = 0; i < frames; i++) out[2 * i] = out[2 * i + 1] = mix[i] * gain;
    }

    void draw_audio(enaga::Canvas &canvas) {
        static const char *const kTones[4] = {"OFF", "750 HZ LEFT", "1500 HZ RIGHT", "750 HZ BOTH"};
        canvas.text(0, 16, kTones[tone_]);
        canvas.text(0, 28, (Line() << "LEVEL " << level_).str(), 10);
        canvas.text(0, 40, "STEP 1L 2R 3LR 4OFF", 6);
        canvas.text(0, 56, (Line() << "IN L " << in_shown_[0]).str());
        canvas.text(0, 68, (Line() << "IN R " << in_shown_[1]).str());
        canvas.rect(0, 80, int(in_shown_[0] / 8), 3, 15);
        canvas.rect(0, 86, int(in_shown_[1] / 8), 3, 15);
    }

    void draw_load(enaga::Canvas &canvas) {
        static const char *const kModes[3] = {"SYNTH", "SAMPLE", "GRAIN"};
        const uint32_t load = load_shown_, wanted = voices_, n = wanted < cap_ ? wanted : cap_;
        canvas.text(0, 16, (Line() << kModes[mode_] << " " << n << (n < wanted ? " CAP" : " VOICES")).str());
        canvas.text(0, 28, (Line() << "LOAD " << load / 10u << "." << load % 10u << "%").str());
        if (n && load) {
            canvas.text(0, 40, (Line() << "CYC/VOICE " << load * (kBlockCycles / 1000u) / n).str());
            canvas.text(0, 52, (Line() << "FIT AT 90% " << n * 900u / load).str());
        }
        canvas.text(0, 68, "STEP 1SYN 2SMP 3GRN", 6);
        canvas.text(0, 78, "5-8: 16 64 128 256", 6);
        canvas.text(0, 86, "VALUE +-1  A +-8", 6);
        canvas.rect(0, 98, int(load > 1000 ? 128 : load * 128 / 1000), 4, 15);
    }

    void draw_panel(enaga::Canvas &canvas) {
        const char *name = kKeyNames[last_key_];
        if (events_) {
            canvas.text(0, 16, (Line() << (name ? name : "BIT") << " " << last_key_ << (last_down_ ? " DOWN" : " UP")).str());
            canvas.text(0, 28, (Line() << "PRESSES " << presses_[last_key_] << " EV " << events_).str(), 10);
        } else {
            canvas.text(0, 16, "PRESS A KEY");
        }
        canvas.text(0, 44, (Line() << "A " << position_[0] << " B " << position_[1]).str());
        canvas.text(0, 56, (Line() << "C " << position_[2] << " D " << position_[3]).str());
        canvas.text(0, 68, (Line() << "VALUE " << position_[4]).str());
        int top = 0;
        for (int pad = 1; pad < 15; pad++)
            if (pad_raw_[pad] > pad_raw_[top]) top = pad;
        canvas.text(0, 84, (Line() << "PAD " << top << " RAW " << pad_raw_[top]).str());
        canvas.text(0, 96, (Line() << "PAD MSG/S " << pads_per_second_).str(), 10);
    }

    // CARD: one stage after another, each moved on by its request callbacks.
    enum CardStage { card_idle, card_writing, card_reading, card_scattering, card_done, card_failed };

    void card_start() {
        if (card_stage_ != card_idle && card_stage_ != card_done && card_stage_ != card_failed) return;
        for (auto &r : card_kibs_) r = 0;
        card_reads_per_s_ = 0;
        card_stage_ = card_writing;
        card_begin(ENAGA_FILE_WRITE);
    }
    void card_begin(uint32_t mode) {          // open the file for the stage and queue its first requests
        int32_t r = card_file_.open(kCardPath, mode);
        if (r < 0) { card_fail(r); return; }
        card_queued_ = card_done_ = 0;
        card_since_ = enaga::ms();
        if (card_stage_ == card_scattering) { card_scatter_next(); return; }
        for (uint32_t i = 0; i < kInFlight; i++) card_queue();
    }
    void card_queue() {
        if (card_queued_ >= kCardBytes) return;
        int32_t r = card_stage_ == card_writing ? card_file_.write<&Check::card_moved>(card_buffer_, kChunk, this)
                                                : card_file_.read<&Check::card_moved>(card_buffer_, kChunk, this);
        if (r < 0) { card_fail(r); return; }
        card_queued_ += kChunk;
    }
    void card_moved(int32_t result) {         // a WRITE or READ request done
        if (card_stage_ != card_writing && card_stage_ != card_reading) return;   // failed meanwhile
        if (result != int32_t(kChunk)) { card_fail(result < 0 ? result : int32_t(ENAGA_FILE_FULL)); return; }
        card_done_ += kChunk;
        if (card_done_ < kCardBytes) { card_queue(); return; }
        int stage = card_stage_ == card_writing ? 0 : 1;
        int32_t r = card_file_.close();       // for a write, the sync: part of its time
        card_kibs_[stage] = kibs(kCardBytes, enaga::ms() - card_since_);
        if (r < 0) { card_fail(r); return; }
        card_stage_ = stage == 0 ? card_reading : card_scattering;
        card_begin(ENAGA_FILE_READ);
    }
    void card_scatter_next() {
        card_random_ = card_random_ * 1664525u + 1013904223u;
        uint32_t at = (card_random_ >> 8) % (kCardBytes - kScatterBytes) & ~1u;   // whole 16-bit samples
        int32_t r = card_file_.seek(at);
        if (r >= 0) r = card_file_.read<&Check::card_scattered>(card_buffer_, kScatterBytes, this);
        if (r < 0) card_fail(r);
    }
    void card_scattered(int32_t result) {
        if (card_stage_ != card_scattering) return;
        if (result != int32_t(kScatterBytes)) { card_fail(result < 0 ? result : int32_t(ENAGA_FILE_CARD_ERROR)); return; }
        if (++card_done_ < kScatterReads) { card_scatter_next(); return; }
        uint32_t ms = enaga::ms() - card_since_;
        card_kibs_[2] = kibs(kScatterReads * kScatterBytes, ms);
        card_reads_per_s_ = kScatterReads * 1000u / (ms ? ms : 1);
        card_file_.close();
        enaga::files::remove(kCardPath);
        card_stage_ = card_done;
        enaga::log((Line() << "CARD W " << card_kibs_[0] << " R " << card_kibs_[1]).str());
        enaga::log((Line() << "CARD S " << card_kibs_[2] << " " << card_reads_per_s_ << "/S").str());
    }
    void card_fail(int32_t error) {
        card_error_ = error;
        card_stage_ = card_failed;
        card_file_.close();                   // cancels whatever is still queued
        enaga::files::remove(kCardPath);
        enaga::log((Line() << "CARD " << enaga::files::error_name(error)).str());
    }
    static uint32_t kibs(uint32_t bytes, uint32_t ms) { return (bytes / 1024u) * 1000u / (ms ? ms : 1); }

    void draw_card(enaga::Canvas &canvas) {
        static const char *const kStages[6] = {"STEP 1: MEASURE", "WRITING", "READING", "SCATTERED READS",
                                               "DONE", "FAILED"};
        canvas.text(0, 16, kStages[card_stage_]);
        if (card_stage_ == card_failed) canvas.text(0, 28, enaga::files::error_name(card_error_), 15);
        else if (card_stage_ == card_writing || card_stage_ == card_reading)
            canvas.text(0, 28, (Line() << card_done_ / 1024u << " KIB").str(), 10);
        else if (card_stage_ == card_scattering) canvas.text(0, 28, (Line() << card_done_ << " READS").str(), 10);
        canvas.text(0, 44, (Line() << "WRITE " << card_kibs_[0] << " KIB/S").str());
        canvas.text(0, 56, (Line() << "READ " << card_kibs_[1] << " KIB/S").str());
        canvas.text(0, 68, (Line() << "SCATTER " << card_kibs_[2] << " KIB/S").str());
        canvas.text(0, 80, (Line() << "  " << card_reads_per_s_ << " READS/S").str(), 10);
        canvas.text(0, 92, (Line() << "STREAMS " << card_kibs_[1] * 2u / 375u).str());
    }

    void leds_off() {
        for (int led = 0; led < ENAGA_LED_COUNT; led++) enaga::host().led(uint32_t(led), 0, 0, 0);
    }

    float table_[kTable + 1] = {};
    int16_t *bank_ = nullptr;
    Synth synth_[kMaxVoices] = {};
    Sample sample_[kMaxVoices] = {};
    uint32_t random_ = 1;                  // audio context only
    uint32_t phase_ = 0;                   // audio context only
    uint32_t uncap_seen_ = 0;              // audio context only

    // Written by the main context, read by audio.
    volatile uint32_t page_ = 0, tone_ = 0, level_ = 25, mode_ = 0, voices_ = 0, uncap_ = 0;
    // Written by audio, read by the main context.
    volatile float in_peak_[2] = {0.0f, 0.0f};
    volatile uint32_t cap_ = kMaxVoices;

    // Main context only.
    bool home_down_ = false, home_used_ = false;
    uint32_t home_since_ = 0;
    uint32_t load_window_ = 0, load_shown_ = 0, load_since_ = 0;
    float in_window_[2] = {0.0f, 0.0f};
    uint32_t in_shown_[2] = {0, 0}, in_since_ = 0;
    int last_key_ = 0;
    bool last_down_ = false;
    uint32_t presses_[ENAGA_KEY_COUNT] = {}, events_ = 0;
    uint32_t pad_raw_[15] = {}, pad_messages_ = 0, pad_messages_last_ = 0, pads_per_second_ = 0;
    int position_[5] = {};
    enaga::files::File card_file_;
    uint8_t *card_buffer_ = nullptr;
    int card_stage_ = card_idle;
    int32_t card_error_ = 0;
    uint32_t card_queued_ = 0, card_done_ = 0, card_since_ = 0, card_random_ = 1;
    uint32_t card_kibs_[3] = {}, card_reads_per_s_ = 0;
};

}  // namespace

ENAGA_APP(Check)
