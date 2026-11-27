// CHECK: the app for sessions on a unit (docs/hardware.md "Checks on a unit"). Six pages; hold F4 for one
// second to go to the next one (the press still counts on the panel page).
//
//   AUDIO  STEP 1: 750 Hz left only, STEP 2: 1500 Hz right only, STEP 3: 750 Hz both, STEP 4: off. VALUE: level.
//          IN L/R: input peaks over the last half second, per mille of full scale. STEP 9-14 choose the input
//          (built-in mic, Hi-Z, dynamic mic, stereo line, mono line, USB) and B its level (LOW, NORMAL, HIGH).
//   LOAD   STEP 1: SYNTH voices (saw + biquad), STEP 2: SAMPLE voices (16-bit playback streaming through an
//          8 MiB bank in DDR), STEP 3: GRAIN voices (as SAMPLE, but each voice jumps to a random place every
//          block). STEP 5-8: 16, 64, 128, 256 voices; VALUE: voices +-1, A: +-8. Shows the highest load in the
//          last second (percent of the block budget), cycles per voice (the load / voices, Enaga's own overhead
//          included), and the voices that would fit in 90 % of the budget at that rate. A block over 90 % cuts the voices to fit 85 % (CAP):
//          past 100 % the callback would take every cycle and the main loop, keys included, would stop.
//   PANEL  the last key by name and bit with its press count (one press must count one: bounce), every encoder's
//          position, the pad with the highest raw reading, and pad messages per second (the panel MCU sends
//          every pad all the time, about 3,930 a second for the 15 pads), and the last pad pressed with its velocity
//          and pressure (ENAGA_EVENT_PAD_PRESSURE, 0-127). A held key lights its LED green; a pad lights green by
//          pressure; A, B and C set VOL L, VOL R and MIC (0-10); VALUE cycles the VALUE LED red, green, blue.
//   CARD   STEP 1 measures the card through Enaga's file service, as an app gets it (queued in the main loop, 2 ms
//          a pass): WRITE, a 4 MiB file to /RECORD/CHECK.BIN in 32 KiB writes, four queued at a time; READ, the
//          file back the same way; SCATTER, 256 reads of 4 KiB at random places in it, one after another (seek,
//          read). KiB/s for each, SCATTER's reads a second, and how many 48 kHz 16-bit stereo streams (187.5
//          KiB/s each) READ's rate would feed. The file is removed afterwards; the results also go to the log.
//          A failure stops the app, so that Enaga's status page shows the log with the SD driver's details.
//   MIDI   the MIDI channel (SETTINGS, MIDI CH), messages in and out, the last four received (hex, then D for the
//          DIN jack or U for USB) and the last
//          SysEx's length. STEP 1 plays key 60 while held (note on / off), STEP 2 sends CC 7 = 100, STEP 3 a
//          SysEx identity request (F0 7E 7F 06 01 F7), STEP 4 starts or stops a MIDI clock at 120 BPM (START,
//          24 clocks a beat, STOP). The CLK line is Enaga's clock (CLOCK SRC, its tempo, RUN and the beat):
//          STEP 5 starts or stops it when CLOCK SRC is INT; CLOCK OUT and SYNC OUT send it.
//   REC    recording and playing back through wav::Writer and a stream, at the card's real pace. STEP 1 records
//          5 s of a test signal (24-bit stereo: a 375 Hz triangle left, 750 Hz right, at -12 dB, heard quietly
//          while it records) to /RECORD/CHECKREC.WAV: the frames written, those lost (the ring was full) and how
//          full the 256 KiB ring got. STEP 2 plays the file back through a stream, heard, comparing every sample
//          with the signal: the frames played, those wrong, and the stream's underruns. STEP 3 records 5 s of the
//          input to /RECORD/CHECKIN.WAV and finds the delay between its channels (LAG, in samples: positive, the
//          right channel late; feed both inputs the same signal, a mono source on the stereo line input).
//          Results also go to the log (REC W, REC P, REC IN).
#include <enaga/app.hpp>
#include <enaga/files.hpp>
#include <enaga/clock.hpp>
#include <enaga/midi.hpp>
#include <enaga/wav.hpp>

#include <cstdint>

namespace {

constexpr int kTable = 64;
constexpr uint32_t kBlockCycles = 496000;          // the 64-frame budget at 372 MHz; load is per mille of it
constexpr uint32_t kBankSamples = 4u << 20;        // 8 MiB of 16-bit samples
constexpr int kMaxVoices = 512;
constexpr int kPageCount = 6;

constexpr const char *kRecPath = "/RECORD/CHECKREC.WAV", *kInPath = "/RECORD/CHECKIN.WAV";
constexpr uint32_t kRecRing = 1u << 18;            // 256 KiB, for recording and for playback
constexpr uint32_t kRecFrames = 5 * 48000;         // 5 s
constexpr int32_t kRecLevel = 1 << 21;             // -12 dB of 24-bit full scale

// The test signal's frame k: triangles of period 128 (left) and 64 (right) frames between -kRecLevel and
// kRecLevel, whole numbers that survive the trip to the card and back exactly.
int32_t rec_triangle(uint32_t k, uint32_t period) {
    int32_t t = int32_t(k % period), p = int32_t(period);
    int32_t distance = 2 * t - p;
    return kRecLevel - (distance < 0 ? -distance : distance) * (2 * kRecLevel / p);
}

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

    void stop() override {                         // a recording still running is closed, a whole WAV file
        rec_mode_ = rec_idle;
        rec_writer_.close();
    }

    void update(uint32_t ms) override {
        if (home_down_ && !home_used_ && ms - home_since_ >= 1000) {
            home_used_ = true;
            leds_off();
            page_ = (page_ + 1) % kPageCount;
        }
        while (clock_on_ && ms - clock_last_ >= (clock_count_ + 1) * 125u / 6u) {   // 120 BPM: 24 a beat,
            clock_count_++;                                                          // 125/6 ms apart
            if (enaga::midi::clock()) midi_out_++;
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
        rec_update();
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
        if (page_ == 4) midi_key(key, down);
        if (page_ == 0 && down && key >= ENAGA_KEY_STEP1 + 8 && key <= ENAGA_KEY_STEP1 + 13) {   // STEP 14: USB
            input_source_ = key - ENAGA_KEY_STEP1 - 8;
            input_result_ = enaga::system::input(input_source_, input_level_);
        }
        if (!down || key < ENAGA_KEY_STEP1 || key > ENAGA_KEY_STEP1 + 7) return;
        static const uint32_t kPresets[4] = {16, 64, 128, 256};
        int step = key - ENAGA_KEY_STEP1;
        if (page_ == 0 && step < 4) tone_ = uint32_t(step + 1) % 4;
        if (page_ == 1 && step < 3) mode_ = uint32_t(step);
        if (page_ == 1 && step >= 4) voices_ = kPresets[step - 4];
        if (page_ == 1) { load_window_ = 0; uncap_ = uncap_ + 1; }
        if (page_ == 3 && step == 0) card_start();
        if (page_ == 5 && step < 3) rec_start(step);
    }

    void on_encoder(int encoder, int delta) override {
        if (encoder >= 0 && encoder < 5) position_[encoder] += delta;
        if (page_ == 0 && encoder == 4) level_ = clamp(int(level_) + 5 * delta, 0, 100);
        if (page_ == 0 && encoder == 1) {
            input_level_ = clamp(input_level_ + delta, 0, 2);
            input_result_ = enaga::system::input(input_source_, input_level_);
        }
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

    void on_pad_press(int pad, int velocity) override { note_pad_ = pad; note_velocity_ = velocity; note_pressure_ = 0; }
    void on_pad_pressure(int pad, int pressure) override { if (pad == note_pad_) note_pressure_ = pressure; }

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
        if (rec_mode_ != rec_idle) rec_audio(in, out, frames);
    }

    void draw(enaga::Canvas &canvas) override {
        static const char *const kPages[kPageCount] = {"AUDIO", "LOAD", "PANEL", "CARD", "MIDI", "REC"};
        canvas.clear();
        canvas.text(0, 0, "CHECK");
        canvas.text(48, 0, kPages[page_], 10);
        canvas.text(0, 118, "HOLD F4: NEXT PAGE", 6);
        if (page_ == 0) draw_audio(canvas);
        if (page_ == 1) draw_load(canvas);
        if (page_ == 2) draw_panel(canvas);
        if (page_ == 3) draw_card(canvas);
        if (page_ == 4) draw_midi(canvas);
        if (page_ == 5) draw_rec(canvas);
    }

    void on_midi(const enaga::MidiMessage &m) override {
        midi_in_++;
        if (m.status == 0xF8) return;                  // clocks are counted, not listed
        for (int i = 3; i > 0; i--) midi_last_[i] = midi_last_[i - 1];
        midi_last_[0] = uint32_t(m.status) | uint32_t(m.data1) << 8 | uint32_t(m.data2) << 16 |
                        uint32_t(m.port == ENAGA_MIDI_USB ? 2 : 1) << 24;
    }
    void on_midi_sysex(int port, const uint8_t *data, uint32_t bytes, bool start, bool end) override {
        (void)port; (void)data;
        sysex_bytes_ = start ? bytes : sysex_bytes_ + bytes;
        if (end) sysex_shown_ = sysex_bytes_;
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
        static const char *const kSources[6] = {"MIC", "HI-Z", "DYN MIC", "LINE ST", "LINE MONO", "USB"};
        static const char *const kLevels[3] = {"LOW", "NORM", "HIGH"};
        canvas.text(0, 98, (Line() << kSources[input_source_] << " " << kLevels[input_level_]
                                   << (input_result_ ? " ERR" : "")).str());
        canvas.text(0, 110, "STEP 9-14 SRC  B LVL", 6);
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
        if (note_pad_ >= 0)
            canvas.text(0, 108, (Line() << "PAD " << note_pad_ << " VEL " << note_velocity_ << " PRS " << note_pressure_).str());
    }

    void midi_key(int key, bool down) {
        int ch = enaga::midi::channel(), step = key - ENAGA_KEY_STEP1;
        bool sent = false;
        if (step == 0) sent = down ? enaga::midi::note_on(ch, 60, 100) : enaga::midi::note_off(ch, 60);
        if (!down) { if (sent) midi_out_++; return; }
        if (step == 1) sent = enaga::midi::control_change(ch, 7, 100);
        if (step == 2) {
            static const uint8_t identity[6] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};
            sent = enaga::midi::sysex(identity, sizeof identity);
        }
        if (step == 3) {
            clock_on_ = !clock_on_;
            sent = clock_on_ ? enaga::midi::start() : enaga::midi::stop();
            clock_last_ = enaga::ms();
            clock_count_ = 0;
        }
        if (step == 4) {
            if (enaga::clock::running()) enaga::clock::stop(); else enaga::clock::start();
        }
        if (sent) midi_out_++;
    }

    void draw_midi(enaga::Canvas &canvas) {
        static const char kHex[] = "0123456789ABCDEF";
        canvas.text(0, 16, (Line() << "CH " << enaga::midi::channel() + 1 << (clock_on_ ? "  CLOCK" : "")).str());
        canvas.text(0, 28, (Line() << "IN " << midi_in_ << " OUT " << midi_out_).str(), 10);
        for (int i = 0; i < 4; i++) {
            uint32_t m = midi_last_[i];
            if (!(m >> 24)) break;
            char text[11] = {kHex[m >> 4 & 15], kHex[m & 15], ' ', kHex[m >> 12 & 15], kHex[m >> 8 & 15], ' ',
                             kHex[m >> 20 & 15], kHex[m >> 16 & 15], ' ', (m >> 24) == 2 ? 'U' : 'D', 0};
            canvas.text(0, 40 + 10 * i, text, i ? 8 : 15);
        }
        canvas.text(0, 82, (Line() << "SYSEX " << sysex_shown_).str(), 10);
        static const char *const kSources[3] = {"INT", "MIDI", "SYNC"};
        enaga::clock::State s = enaga::clock::state();
        int tenths = int(s.tempo * 10.0f + 0.5f);
        canvas.text(0, 92, (Line() << "CLK " << kSources[s.source % 3] << " " << tenths / 10 << "." << tenths % 10
                                   << (s.running ? " RUN " : " ") << (s.running ? s.position / 96 + 1 : 0u)).str());
        canvas.text(0, 102, "1 NOTE 2 CC 3 SYSEX", 6);
        canvas.text(0, 110, "4 OWN CLOCK 5 ENAGA CLK", 6);
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
        // To Enaga's status page on purpose: it shows the log, where Enaga notes the SD driver's error details.
        enaga::fatal("CARD TEST FAILED");
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

    // REC: the audio context does the signal and the comparing; the main context opens, closes and reports.
    enum RecMode : uint32_t { rec_idle, rec_test, rec_filling, rec_playing, rec_input, rec_finished };

    void rec_start(int step) {
        if (rec_mode_ != rec_idle) return;
        rec_ring_ = rec_ring_ ? rec_ring_ : new uint8_t[kRecRing];
        rec_frame_ = 0;
        rec_buffered_max_ = 0;
        rec_error_ = 0;
        if (step == 0 || step == 2) {
            enaga::wav::Format format;
            format.bits = 24;
            int32_t r = rec_writer_.create(step == 0 ? kRecPath : kInPath, format, rec_ring_, kRecRing);
            if (r != ENAGA_FILE_OK) { rec_fail(r); return; }
            rec_written_ = rec_lost_ = 0;
            for (auto &c : rec_correlation_) c = 0.0f;
            for (auto &h : rec_history_) h = 0.0f;
            rec_input_lag_ = 99;
            rec_input_take_ = step == 2;
            rec_mode_ = step == 0 ? rec_test : rec_input;    // last: audio() starts from here
            enaga::system::set_busy(true);
            return;
        }
        // STEP 2: the test recording, checked as it plays. Its header first (read in the background), then the
        // stream from where the samples start.
        int32_t r = rec_file_.open(kRecPath, ENAGA_FILE_READ);
        if (r == ENAGA_FILE_OK) r = rec_file_.read<&Check::rec_header_read>(rec_head_, sizeof rec_head_, this);
        if (r < 0) { rec_file_.close(); rec_fail(r); return; }
        rec_mode_ = rec_filling;
        enaga::system::set_busy(true);
    }

    void rec_header_read(int32_t got) {
        enaga::wav::Format format;
        rec_file_.close();
        if (got < 0) { rec_fail(got); return; }
        if (!enaga::wav::parse(rec_head_, uint32_t(got), format) || format.bits != 24 || format.channels != 2) {
            rec_fail(enaga::wav::not_wav);
            return;
        }
        rec_play_frames_ = format.data_bytes / 6u;
        rec_played_ = 0;
        rec_bad_ = 0;
        rec_underruns_ = 0;
        rec_carry_ = 0;
        int32_t r = rec_stream_.open(kRecPath, format.data_offset, rec_ring_, kRecRing);
        if (r != ENAGA_FILE_OK) rec_fail(r);
    }

    void rec_fail(int32_t error) {                // reported, and back to idle, by rec_update
        rec_error_ = error;
        rec_mode_ = rec_finished;
    }

    void rec_update() {                           // the main context's part, every pass
        const uint32_t mode = rec_mode_;
        if (mode == rec_test || mode == rec_input) {
            enaga::files::Stream::Status s = rec_writer_.status();
            if (s.buffered > rec_buffered_max_) rec_buffered_max_ = s.buffered;
            if (s.state < 0) { rec_error_ = s.state; rec_mode_ = rec_finished; }
        }
        if (mode == rec_filling && rec_stream_.is_open() && rec_stream_.ready()) rec_mode_ = rec_playing;
        if (mode == rec_playing) {                // a failure, or a file shorter than its header said
            enaga::files::Stream::Status s = rec_stream_.status();
            if (s.state < 0) rec_error_ = s.state;
            if (s.state < 0 || (s.state == ENAGA_STREAM_ENDED && !s.buffered)) rec_mode_ = rec_finished;
        }
        if (rec_mode_ != rec_finished) return;
        if (rec_writer_.is_open()) {              // a recording done (or failed)
            rec_written_ = rec_writer_.frames();
            rec_lost_ = rec_writer_.frames_lost();
            int32_t r = rec_writer_.close();
            if (r < 0 && !rec_error_) rec_error_ = r;
            if (rec_input_take_) {
                int best = 0;
                for (int d = -2; d <= 2; d++) if (rec_correlation_[d + 2] > rec_correlation_[best + 2]) best = d;
                rec_input_lag_ = best;
                enaga::log((Line() << "REC IN LAG " << best << " W " << rec_written_ << " L " << rec_lost_).str());
            } else {
                enaga::log((Line() << "REC W " << rec_written_ << " L " << rec_lost_ << " R "
                                   << rec_buffered_max_ * 100u / kRecRing << "%").str());
            }
        }
        if (rec_stream_.is_open()) {              // a playback done (or failed)
            rec_underruns_ = rec_stream_.status().underruns;
            rec_stream_.close();
            enaga::log((Line() << "REC P " << rec_played_ << " B " << rec_bad_ << " U " << rec_underruns_).str());
        }
        if (rec_error_) enaga::log((Line() << "REC " << enaga::files::error_name(rec_error_)).str());
        enaga::system::set_busy(false);
        rec_mode_ = rec_idle;
    }

    void rec_audio(const float *in, float *out, uint32_t frames) {
        const uint32_t mode = rec_mode_;
        if (frames > ENAGA_BLOCK_FRAMES) frames = ENAGA_BLOCK_FRAMES;
        if (mode == rec_test) {
            float block[2 * ENAGA_BLOCK_FRAMES];
            for (uint32_t i = 0; i < frames; i++) {
                block[2 * i] = float(rec_triangle(rec_frame_ + i, 128)) * (1.0f / 8388608.0f);
                block[2 * i + 1] = float(rec_triangle(rec_frame_ + i, 64)) * (1.0f / 8388608.0f);
                out[2 * i] = block[2 * i] * 0.25f;
                out[2 * i + 1] = block[2 * i + 1] * 0.25f;
            }
            rec_writer_.write(block, frames);
            rec_frame_ += frames;
            if (rec_frame_ >= kRecFrames) rec_mode_ = rec_finished;
        } else if (mode == rec_input) {
            rec_writer_.write(in, frames);
            // c[d + 2] sums L[n - d] R[n] for d = 0..2 (the right channel late) and L[n] R[n + d] for d = -1, -2.
            float l1 = rec_history_[0], l2 = rec_history_[1], r1 = rec_history_[2], r2 = rec_history_[3];
            float c[5] = {};
            for (uint32_t i = 0; i < frames; i++) {
                float l = in[2 * i], r = in[2 * i + 1];
                c[2] += l * r;
                c[3] += l1 * r;
                c[4] += l2 * r;
                c[1] += l * r1;
                c[0] += l * r2;
                l2 = l1; l1 = l; r2 = r1; r1 = r;
            }
            rec_history_[0] = l1; rec_history_[1] = l2; rec_history_[2] = r1; rec_history_[3] = r2;
            for (int d = 0; d < 5; d++) rec_correlation_[d] += c[d];
            rec_frame_ += frames;
            if (rec_frame_ >= kRecFrames) rec_mode_ = rec_finished;
        } else if (mode == rec_playing) {
            uint8_t bytes[6 * ENAGA_BLOCK_FRAMES];
            uint32_t got = rec_stream_.read(bytes, 6 * frames), done = 0;
            for (uint32_t i = 0; i < got; i++) {     // whole frames, a frame cut by the ring kept for next time
                rec_carry_bytes_[rec_carry_++] = bytes[i];
                if (rec_carry_ < 6) continue;
                rec_carry_ = 0;
                const uint8_t *p = rec_carry_bytes_;
                int32_t l = int32_t(uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 24) >> 8;
                int32_t r = int32_t(uint32_t(p[3]) << 8 | uint32_t(p[4]) << 16 | uint32_t(p[5]) << 24) >> 8;
                const uint32_t k = rec_played_;
                if (l != rec_triangle(k, 128) || r != rec_triangle(k, 64)) rec_bad_ = rec_bad_ + 1;
                if (done < frames) {
                    out[2 * done] = float(l) * (0.25f / 8388608.0f);
                    out[2 * done + 1] = float(r) * (0.25f / 8388608.0f);
                    done++;
                }
                rec_played_ = k + 1;
            }
            if (rec_played_ >= rec_play_frames_) rec_mode_ = rec_finished;
        }
    }

    void draw_rec(enaga::Canvas &canvas) {
        static const char *const kModes[6] = {"STEP 1 2 3", "RECORDING", "FILLING", "PLAYING", "RECORDING IN",
                                              "FINISHING"};
        const uint32_t mode = rec_mode_;
        uint32_t frames = mode == rec_playing ? rec_played_ : mode == rec_idle ? 0u : rec_frame_;
        canvas.text(0, 16, kModes[mode < 6 ? mode : 0]);
        if (frames) canvas.text(84, 16, (Line() << frames / 48000u << "." << frames % 48000u / 4800u << " S").str(), 10);
        if (rec_error_) canvas.text(0, 28, enaga::files::error_name(rec_error_), 15);
        canvas.text(0, 40, (Line() << "WROTE " << rec_written_ << " LOST " << rec_lost_).str());
        canvas.text(0, 52, (Line() << "RING MAX " << rec_buffered_max_ * 100u / kRecRing << "%").str(), 10);
        canvas.text(0, 64, (Line() << "PLAYED " << rec_played_ << " BAD " << rec_bad_).str());
        canvas.text(0, 76, (Line() << "UNDERRUNS " << rec_underruns_).str(), 10);
        if (rec_input_lag_ < 99) canvas.text(0, 88, (Line() << "IN LAG " << rec_input_lag_).str());
        canvas.text(0, 100, "1 REC TEST  2 PLAY+CHECK", 6);
        canvas.text(0, 108, "3 REC INPUT (LAG)", 6);
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
    uint32_t midi_in_ = 0, midi_out_ = 0, midi_last_[4] = {}, sysex_bytes_ = 0, sysex_shown_ = 0;
    bool clock_on_ = false;
    uint32_t clock_last_ = 0, clock_count_ = 0;
    int input_source_ = ENAGA_INPUT_LINE_STEREO, input_level_ = ENAGA_INPUT_NORMAL, input_result_ = 0;
    int note_pad_ = -1, note_velocity_ = 0, note_pressure_ = 0;
    uint32_t pad_raw_[15] = {}, pad_messages_ = 0, pad_messages_last_ = 0, pads_per_second_ = 0;
    int position_[5] = {};
    enaga::files::File card_file_;
    uint8_t *card_buffer_ = nullptr;
    int card_stage_ = card_idle;
    int32_t card_error_ = 0;
    uint32_t card_queued_ = 0, card_done_ = 0, card_since_ = 0, card_random_ = 1;
    uint32_t card_kibs_[3] = {}, card_reads_per_s_ = 0;

    // REC. Written by the main context before a mode starts, then by audio until it is finished; rec_mode_ by both
    // in turn (the main context starts a mode, audio finishes it, the main context returns it to idle).
    volatile uint32_t rec_mode_ = rec_idle;
    volatile uint32_t rec_frame_ = 0, rec_played_ = 0, rec_bad_ = 0;
    uint32_t rec_play_frames_ = 0, rec_carry_ = 0;
    uint8_t rec_carry_bytes_[6] = {};
    float rec_history_[4] = {};
    volatile float rec_correlation_[5] = {};
    enaga::wav::Writer rec_writer_;
    enaga::files::Stream rec_stream_;
    enaga::files::File rec_file_;
    uint8_t rec_head_[64] = {};
    uint8_t *rec_ring_ = nullptr;
    // Main context only.
    uint32_t rec_written_ = 0, rec_lost_ = 0, rec_buffered_max_ = 0, rec_underruns_ = 0;
    int32_t rec_error_ = 0;
    int rec_input_lag_ = 99;
    bool rec_input_take_ = false;
};

}  // namespace

ENAGA_APP(Check)
