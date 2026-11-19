// Stream test app (tests/test_streams.py). The card holds /SAMPLES/LONG.WAV (16-bit stereo, 1 MiB of data after a
// LIST chunk; 16-bit word j of the data is j * 7 + (j >> 11)) and /SAMPLES/SHORT.WAV (6,000 bytes of data, same
// pattern). The app plays four streams from LONG.WAV at 128 KiB apart and one from SHORT.WAV, checking every
// word in audio(), and records /RECORD/TAKE.WAV from audio() (word j: j * 13 + 5) for 1.5 s. Then it writes one
// line per check, "<check> <result>", to RESULTS.TXT in its folder and logs "STREAMS DONE".
#include <enaga/files.hpp>
#include <enaga/wav.hpp>

#include <cstdint>

namespace {
namespace files = enaga::files;
namespace wav = enaga::wav;

constexpr int kLong = 4;
constexpr uint32_t kRing = 1u << 16, kShortRing = 4096, kBlocks = 1125;   // 1.5 s of 64-frame blocks
constexpr uint32_t kApart = 128 * 1024;
uint16_t played(uint32_t j) { return uint16_t(j * 7 + (j >> 11)); }
uint16_t recorded(uint32_t j) { return uint16_t(j * 13 + 5); }
}  // namespace

class Streams : public enaga::App {
public:
    bool init() override {
        for (auto &r : rings_) r = new uint8_t[kRing];
        short_ring_ = new uint8_t[kShortRing];
        take_ring_ = new uint8_t[kRing];
        return take_ring_ != nullptr;
    }

    void audio(const float *, float *out, uint32_t frames) override {
        for (uint32_t i = 0; i < 2 * frames; i++) out[i] = 0.0f;
        if (!playing_) return;
        uint16_t buffer[128];
        for (int k = 0; k < kLong; k++) {
            uint32_t got = long_[k].read(buffer, sizeof buffer);
            for (uint32_t i = 0; i < got / 2; i++)
                if (buffer[i] != played(first_[k] + words_[k] + i)) mismatches_[k]++;
            words_[k] += got / 2;
        }
        uint32_t got = short_.read(buffer, sizeof buffer);
        for (uint32_t i = 0; i < got / 2; i++)
            if (buffer[i] != played(short_words_ + i)) short_mismatches_++;
        short_words_ += got / 2;
        for (uint32_t i = 0; i < 128; i++) buffer[i] = recorded(taken_words_ + i);
        taken_words_ += take_.write(buffer, sizeof buffer) / 2;
        if (++blocks_ == kBlocks) playing_ = false;
    }

    void update(uint32_t) override {
        switch (phase_) {
        case 0:
            head_file_.open("/SAMPLES/LONG.WAV", ENAGA_FILE_READ);
            head_file_.read<&Streams::head_read>(head_, sizeof head_, this);
            phase_ = 1;
            break;
        case 1: break;                                       // head_read() moves on
        case 2: {
            bool all = short_.ready();
            for (auto &s : long_) all = all && s.ready();
            if (all) { playing_ = true; phase_ = 3; }
            break;
        }
        case 3:
            if (!playing_) phase_ = 4;
            break;
        case 4: finish(); phase_ = 99; break;
        default: break;
        }
    }

private:
    void head_read(int32_t result) {
        head_file_.close();
        wav::Format format;
        check("parse", result > 0 && wav::parse(head_, uint32_t(result), format) ? 1 : 0);
        check("data_offset", int32_t(format.data_offset));
        check("data_bytes", int32_t(format.data_bytes));
        for (int k = 0; k < kLong; k++) {
            first_[k] = k * kApart / 2;
            check("open_long", long_[k].open("/SAMPLES/LONG.WAV", format.data_offset + k * kApart, rings_[k], kRing));
        }
        check("open_short", short_.open("/SAMPLES/SHORT.WAV", 44, short_ring_, kShortRing));
        check("create_take", take_.create("/RECORD/TAKE.WAV", 44, take_ring_, kRing));
        {
            files::Stream bad;
            check("bad_ring", bad.open("/SAMPLES/LONG.WAV", 0, rings_[0], 5000));
            check("missing", bad.open("/SAMPLES/NONE.WAV", 0, rings_[0], kRing));
            check("denied", bad.create("/X.WAV", 0, rings_[0], kRing));
        }
        uint16_t word = 0;
        check("write_on_playback", int32_t(long_[0].write(&word, 2)));
        check("read_on_recording", int32_t(take_.read(&word, 2)));
        phase_ = 2;
    }

    void finish() {
        for (int k = 0; k < kLong; k++) {
            auto s = long_[k].status();
            check("long_mismatches", mismatches_[k]);
            check("long_underruns", int32_t(s.underruns));
            check("long_position", int32_t(s.position));
            check("long_state", s.state);
        }
        auto s = short_.status();
        check("short_mismatches", short_mismatches_);
        check("short_position", int32_t(s.position));
        check("short_underruns", int32_t(s.underruns));
        check("short_state", s.state);
        auto t = take_.status();
        check("take_underruns", int32_t(t.underruns));
        check("take_position", int32_t(t.position));
        uint8_t header[44];
        wav::Format format;
        wav::header(header, format, t.position);
        check("take_close", take_.close(header, sizeof header));
        for (auto &l : long_) l.close();
        short_.close();
        char path[64];
        join(path, files::data_folder(), "/RESULTS.TXT");
        enaga::log(files::save(path, results_, used_) == ENAGA_FILE_OK ? "STREAMS DONE" : "STREAMS SAVE FAILED");
    }

    void check(const char *name, int32_t value) {
        uint32_t i = 0;
        while (name[i] && used_ < sizeof results_ - 16) results_[used_++] = name[i++];
        results_[used_++] = ' ';
        char digits[12];
        int n = 0;
        uint32_t v = value < 0 ? uint32_t(-value) : uint32_t(value);
        do digits[n++] = char('0' + v % 10); while (v /= 10);
        if (value < 0) results_[used_++] = '-';
        while (n) results_[used_++] = digits[--n];
        results_[used_++] = '\n';
    }
    static void join(char *out, const char *a, const char *b) {
        while (*a) *out++ = *a++;
        while (*b) *out++ = *b++;
        *out = 0;
    }

    uint8_t *rings_[kLong] = {}, *short_ring_ = nullptr, *take_ring_ = nullptr;
    files::Stream long_[kLong], short_, take_;
    files::File head_file_;
    uint8_t head_[512] = {};
    uint32_t first_[kLong] = {}, words_[kLong] = {}, short_words_ = 0, taken_words_ = 0, blocks_ = 0;
    int32_t mismatches_[kLong] = {}, short_mismatches_ = 0;
    volatile bool playing_ = false;
    int phase_ = 0;
    char results_[2048] = {};
    uint32_t used_ = 0;
};

ENAGA_APP(Streams)
