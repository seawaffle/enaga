// wav::Writer test app (tests/test_streams.py): a ring kept full on purpose. Two 24-bit stereo files (6-byte
// frames, which a 4 KiB ring cuts in the middle when it fills) are written from update() in bursts larger than
// the ring, so most frames are lost and many writes end part-way through a frame. Frame k holds k on the left and
// -k on the right, so the file shows whether only whole frames went in, in order. FULL.WAV through write() (floats,
// k / 2^23), BYTES.WAV through write_bytes() (the 24-bit bytes). Then RESULTS.TXT in its folder, one line per
// check, "<check> <result>", and the log line "WRITER DONE".
#include <enaga/files.hpp>
#include <enaga/wav.hpp>

#include <cstdint>

namespace {
namespace files = enaga::files;
namespace wav = enaga::wav;

constexpr uint32_t kRing = 4096, kBurst = 700, kTotal = 42 * kBurst;   // a burst: 4,200 bytes
}  // namespace

class Writer : public enaga::App {
public:
    bool init() override {
        wav::Format format;
        format.bits = 24;
        ring_[0] = new uint8_t[kRing];
        ring_[1] = new uint8_t[kRing];
        result("create_full", files_[0].create("/RECORD/FULL.WAV", format, ring_[0], kRing));
        result("create_bytes", files_[1].create("/RECORD/BYTES.WAV", format, ring_[1], kRing));
        wav::Format odd = format;
        odd.bits = 12;
        wav::Writer bad;
        result("create_12_bit", bad.create("/RECORD/BAD.WAV", odd, ring_[0], kRing));
        return true;
    }

    void update(uint32_t) override {
        if (done_) return;
        if (next_ < kTotal) {
            for (uint32_t i = 0; i < kBurst; i++) {
                const int32_t k = int32_t(next_ + i);
                floats_[2 * i] = float(k) * (1.0f / 8388608.0f);
                floats_[2 * i + 1] = -floats_[2 * i];
                const uint32_t l = uint32_t(k), r = uint32_t(-k);
                uint8_t *p = bytes_ + 6 * i;
                p[0] = uint8_t(l); p[1] = uint8_t(l >> 8); p[2] = uint8_t(l >> 16);
                p[3] = uint8_t(r); p[4] = uint8_t(r >> 8); p[5] = uint8_t(r >> 16);
            }
            taken_[0] += files_[0].write(floats_, kBurst);
            taken_[1] += files_[1].write_bytes(bytes_, 6 * kBurst);
            next_ += kBurst;
            return;
        }
        for (int f = 0; f < 2; f++) {
            const char *names[2][4] = {{"full_frames", "full_lost", "full_taken", "full_close"},
                                       {"bytes_frames", "bytes_lost", "bytes_taken", "bytes_close"}};
            result(names[f][0], int32_t(files_[f].frames()));
            result(names[f][1], int32_t(files_[f].frames_lost()));
            result(names[f][2], int32_t(taken_[f]));
            result(names[f][3], files_[f].close());
        }
        result("closed_again", files_[0].close());
        files::save("/ENAGA/DATA/WRITER/RESULTS.TXT", text_, length_);
        enaga::log("WRITER DONE");
        done_ = true;
    }

private:
    void result(const char *name, int32_t value) {
        for (const char *p = name; *p && length_ < sizeof text_ - 16; p++) text_[length_++] = *p;
        text_[length_++] = ' ';
        if (value < 0) { text_[length_++] = '-'; value = -value; }
        char digits[12];
        int n = 0;
        do { digits[n++] = char('0' + value % 10); value /= 10; } while (value);
        while (n) text_[length_++] = digits[--n];
        text_[length_++] = '\n';
    }

    wav::Writer files_[2];
    uint8_t *ring_[2] = {};
    float floats_[2 * kBurst] = {};
    uint8_t bytes_[6 * kBurst] = {};
    uint32_t next_ = 0, taken_[2] = {};
    bool done_ = false;
    char text_[1024] = {};
    uint32_t length_ = 0;
};

ENAGA_APP(Writer)
