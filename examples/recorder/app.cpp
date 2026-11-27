// Recorder: an example app. Records the unit's input (whatever the INPUT setting chooses) to a WAV file in
// /RECORD, TAKE001.WAV, TAKE002.WAV and so on, the first free name each time. OK starts and stops a take; F1
// chooses the format between takes (16-bit, 24-bit or 32-bit float, stereo at 48 kHz); VALUE sets how loud the
// input is heard (the monitor, off at first: with the built-in mic, the speaker would feed back). Shows the
// take's name and length, the input's level, and the frames lost if the card ever fell behind.
//
// wav::Writer does the file: create() in the main context with a ring from the heap, write() in audio() with the
// input as it comes, close() in the main context, which writes the header with the final size. stop() closes a
// take still running, so switching apps or switching off leaves a whole WAV file.
#include <enaga/app.hpp>
#include <enaga/files.hpp>
#include <enaga/ui.hpp>
#include <enaga/wav.hpp>

#include <cstdint>

namespace {

constexpr uint32_t kRingBytes = 1u << 18;      // 256 KiB: 0.9 s of 24-bit stereo, for the card's slow moments
constexpr int kFormats = 3;
const char *const kFormatNames[kFormats] = {"16-BIT", "24-BIT", "FLOAT"};

class Recorder : public enaga::App {
public:
    bool init() override {
        ring_ = new uint8_t[kRingBytes];
        enaga::log("recorder ready");
        return true;
    }

    void stop() override { finish(); }

    void on_key(int key, bool down) override {
        if (!down) return;
        if (key == ENAGA_KEY_OK) {
            if (writer_.is_open()) finish(); else start();
        }
        if (key == ENAGA_KEY_F1 && !writer_.is_open()) format_ = (format_ + 1) % kFormats;
    }

    void on_encoder(int encoder, int delta) override {
        if (encoder != 4) return;
        int level = int(monitor_) + delta * 5;
        monitor_ = uint32_t(level < 0 ? 0 : level > 100 ? 100 : level);
    }

    void update(uint32_t ms) override {
        for (int c = 0; c < 2; c++) {                  // each block's peak, held for a second as a mark
            float level = block_peak_[c];
            level_[c] = level;
            if (level >= hold_[c] || ms - hold_since_[c] > 1000) { hold_[c] = level; hold_since_[c] = ms; }
        }
        if (writer_.is_open() && writer_.status().state < 0) {   // the card filled up or failed: keep what we have
            int32_t error = writer_.status().state;
            finish();
            notice_.show(enaga::files::error_name(error));
        }
    }

    void audio(const float *in, float *out, uint32_t frames) override {
        float peak[2] = {0.0f, 0.0f};
        const float monitor = float(monitor_) * 0.01f;
        for (uint32_t i = 0; i < 2 * frames; i++) {
            float a = in[i] < 0.0f ? -in[i] : in[i];
            if (a > peak[i & 1]) peak[i & 1] = a;
            out[i] = in[i] * monitor;
        }
        block_peak_[0] = peak[0];
        block_peak_[1] = peak[1];
        if (recording_) writer_.write(in, frames);
    }

    void draw(enaga::Canvas &canvas) override {
        using enaga::ui::Line;
        canvas.clear();
        enaga::ui::status_line(canvas, "RECORDER", recording_ ? "REC" : kFormatNames[format_]);
        canvas.text(0, 16, name_[0] ? name_ : "OK: RECORD", recording_ ? 15 : 10);
        const uint32_t seconds = writer_.frames() / ENAGA_SAMPLE_RATE;
        const uint32_t tenths = writer_.frames() % ENAGA_SAMPLE_RATE / (ENAGA_SAMPLE_RATE / 10);
        canvas.text(0, 28, Line().number(int(seconds / 60), 2, '0').add(':').number(int(seconds % 60), 2, '0')
                               .add('.').number(int(tenths)));
        for (int c = 0; c < 2; c++) {
            canvas.text(0, 56 + 10 * c, c ? "R" : "L", 10);
            enaga::ui::meter(canvas, 10, 57 + 10 * c, 118, 5, enaga::ui::meter_db(level_[c]),
                             enaga::ui::meter_db(hold_[c]));
        }
        Line monitor("MONITOR ");
        if (monitor_) monitor.number(int(monitor_)).add('%'); else monitor.add("OFF");
        canvas.text(0, 82, monitor, 10);
        if (writer_.frames_lost()) canvas.text(0, 94, Line("LOST ").number(int(writer_.frames_lost())).add(" FRAMES"));
        enaga::ui::hint_line(canvas, recording_ ? "OK: STOP" : "OK: REC  F1: FORMAT");
        notice_.draw(canvas);
    }

private:
    void start() {
        static const uint16_t kBits[kFormats] = {16, 24, 32};
        enaga::wav::Format format;                    // stereo at 48 kHz
        format.encoding = format_ == 2 ? enaga::wav::floating : enaga::wav::pcm;
        format.bits = kBits[format_];
        enaga::files::Info info;
        do {                                          // the first free TAKEnnn.WAV
            path_.clear();
            path_.add("/RECORD/TAKE").number(int(take_++), 3, '0').add(".WAV");
        } while (take_ <= 999 && enaga::files::info(path_, info) == ENAGA_FILE_OK);
        int32_t r = writer_.create(path_, format, ring_, kRingBytes);
        if (r != ENAGA_FILE_OK) { notice_.show(enaga::files::error_name(r)); return; }
        path_.copy_to(name_, sizeof name_);
        recording_ = true;                            // last: audio() starts writing from here
        enaga::system::set_busy(true);
        enaga::log(name_ + 8);
    }

    void finish() {
        if (!writer_.is_open()) return;
        recording_ = false;                           // first: audio() stops writing
        int32_t r = writer_.close();
        enaga::system::set_busy(false);
        notice_.show(r == ENAGA_FILE_OK ? "SAVED" : enaga::files::error_name(r));
    }

    uint8_t *ring_ = nullptr;
    enaga::wav::Writer writer_;
    // Written by the main context, read by audio.
    volatile bool recording_ = false;
    volatile uint32_t monitor_ = 0;
    // Written by audio, read by the main context.
    volatile float block_peak_[2] = {0.0f, 0.0f};
    // Main context only.
    float level_[2] = {}, hold_[2] = {};
    uint32_t hold_since_[2] = {};
    uint32_t format_ = 1;                             // 24-bit
    uint32_t take_ = 1;
    enaga::ui::Line path_;
    char name_[24] = {};
    enaga::ui::Notice notice_;
};

}  // namespace

ENAGA_APP(Recorder)
