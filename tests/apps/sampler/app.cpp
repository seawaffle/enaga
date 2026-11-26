// Loader check app: starts a load and cancels it, then loads /SAMPLES/0.WAV to 9.WAV one after another with
// enaga::wav::Loader (7.WAV keeping its rate), logs each result and some frames for tests/test_sampler.py, then
// plays 0.WAV through a SamplePlayer.
// Log lines: "L<n> <result> <channels> <rate>", then "V<n> <frame> <channel> <value x 1e6>" for each frame asked.
#include <enaga/app.hpp>
#include <enaga/dsp.hpp>
#include <enaga/wav.hpp>

namespace {

constexpr int kFiles = 10;
constexpr uint32_t kFrames[] = {0, 1000, 5000};

void append(char *&out, const char *text) { while (*text) *out++ = *text++; }
void append(char *&out, int32_t value) {
    char digits[12];
    int n = 0;
    uint32_t magnitude = value < 0 ? uint32_t(-value) : uint32_t(value);
    do { digits[n++] = char('0' + magnitude % 10); magnitude /= 10; } while (magnitude);
    if (value < 0) *out++ = '-';
    while (n) *out++ = digits[--n];
}

}  // namespace

class Sampler : public enaga::App {
public:
    bool init() override {                                  // a load cancelled at once first: no callback
        loader_.load<&Sampler::loaded>("/SAMPLES/0.WAV", samples_[0], this);
        loader_.cancel();
        enaga::log(loader_.busy() ? "CANCEL BUSY" : "CANCEL IDLE");
        return true;
    }

    void update(uint32_t) override {
        if (cancelling_ && !loader_.busy()) {               // the file service has handed back the buffer
            cancelling_ = false;
            enaga::log("CANCELLED");
            next();
        }
    }

    void audio(const float *, float *out, uint32_t frames) override {
        float left[ENAGA_BLOCK_FRAMES] = {}, right[ENAGA_BLOCK_FRAMES] = {};
        if (playing_) player_.render_add(left, right, int(frames), 1.0f, 1.0f, 1.0f);
        for (uint32_t i = 0; i < frames; i++) {
            out[2 * i] = left[i];
            out[2 * i + 1] = right[i];
        }
    }

private:
    void next() {
        while (index_ < kFiles) {
            char path[16] = "/SAMPLES/0.WAV";
            path[9] = char('0' + index_);
            int32_t r = loader_.load<&Sampler::loaded>(path, samples_[index_], this, index_ != 7);
            if (r >= 0) return;
            line("L", index_, r, 0, 0);                     // failed at once: no callback
            index_++;
        }
        player_.start(samples_[0]);
        player_.set_loop(enaga::dsp::SamplePlayer::forward, 0, samples_[0].frames());
        playing_ = true;
        enaga::log("PLAYING");
    }

    void loaded(int32_t result) {
        const enaga::dsp::Sample &s = samples_[index_];
        line("L", index_, result, int32_t(s.channels()), int32_t(s.rate()));
        for (uint32_t frame : kFrames)
            for (uint32_t c = 0; c < s.channels(); c++)
                if (frame < s.frames()) {
                    float v = s.data()[frame * s.channels() + c] * 1e6f;
                    line("V", index_, int32_t(frame), int32_t(c), int32_t(v < 0 ? v - 0.5f : v + 0.5f));
                }
        index_++;
        next();
    }

    void line(const char *kind, int n, int32_t a, int32_t b, int32_t c) {
        char text[64], *out = text;
        append(out, kind);
        append(out, n);
        const int32_t values[] = {a, b, c};
        for (int32_t v : values) {
            *out++ = ' ';
            append(out, v);
        }
        *out = 0;
        enaga::log(text);
    }

    enaga::wav::Loader loader_;
    enaga::dsp::Sample samples_[kFiles];
    enaga::dsp::SamplePlayer player_;
    volatile bool playing_ = false;
    bool cancelling_ = true;
    int index_ = 0;
};

ENAGA_APP(Sampler)
