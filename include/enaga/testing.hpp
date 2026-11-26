// Enaga: testing an app's sound code on a PC. tools/host_test.py builds an app folder's tests (tests/*.cpp) with
// its sources, the DSP library and a stand-in for Enaga's host table (runtime/host/host.cpp), and runs them; this
// header has what tests need: test signals, measurements, checks, and WAV files to listen to or to feed in.
//
//     #include <enaga/testing.hpp>
//     #include "../engine.hpp"
//     using namespace enaga::testing;
//
//     int main() {
//         Checks t;
//         Engine engine;
//         Buffer out = render(48000, [&](float *left, float *right, int n) { engine.render(left, right, n); });
//         t.below("never clips", peak(out), 1.0);
//         t.near("plays at 440 Hz", frequency(channel(out, 0)), 440, 1);   // render gives interleaved stereo
//         write_wav(output("engine.wav"), out);                    // to listen to: the test's build folder
//         return t.finish();
//     }
//
// The DSP library computes the same samples on the PC as on Enaga (the fast math in enaga::dsp too); <cmath>'s
// functions are the PC's, which can differ from Enaga's in the last bits. For the PC only: not for app builds.
#pragma once

#if defined(__TMS320C6X__) || defined(_TMS320C6X)
#error "<enaga/testing.hpp> is for tests on a PC (tools/host_test.py), not for apps"
#endif

#include <enaga/abi.h>
#include <enaga/dsp.h>
#include <enaga/wav.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace enaga::testing {

// Audio as the tests handle it: float samples, mono, or stereo interleaved (L R L R) where a function says so.
using Buffer = std::vector<float>;

constexpr float rate = float(ENAGA_SAMPLE_RATE);

namespace detail {
// Output a line at a time, so what a test printed before a crash is not lost in the buffer.
inline const bool line_buffered = (std::setvbuf(stdout, nullptr, _IOLBF, 0), true);
}  // namespace detail

// ---- Where files go ---------------------------------------------------------------------------------------------

// A file in the app's tests/ folder (inputs: a WAV to feed in), and one in the tests' build folder (outputs).
inline std::string input(const std::string &name) {
#ifdef ENAGA_TEST_INPUT
    return std::string(ENAGA_TEST_INPUT) + "/" + name;
#else
    return name;
#endif
}
inline std::string output(const std::string &name) {
#ifdef ENAGA_TEST_OUTPUT
    return std::string(ENAGA_TEST_OUTPUT) + "/" + name;
#else
    return name;
#endif
}

// ---- Signals ----------------------------------------------------------------------------------------------------

inline Buffer silence(size_t frames) { return Buffer(frames, 0.0f); }
inline Buffer sine(float hz, size_t frames, float amplitude = 1.0f, float phase = 0.0f) {
    Buffer b(frames);
    for (size_t i = 0; i < frames; i++) b[i] = amplitude * float(std::sin(2 * M_PI * hz * double(i) / rate + phase));
    return b;
}
// 1 at `at`, 0 elsewhere.
inline Buffer impulse(size_t frames, size_t at = 0, float height = 1.0f) {
    Buffer b(frames, 0.0f);
    if (at < frames) b[at] = height;
    return b;
}
// White noise, -amplitude to amplitude; the same for the same seed.
inline Buffer noise(size_t frames, float amplitude = 1.0f, uint32_t seed = 1) {
    Buffer b(frames);
    uint32_t state = seed ? seed : 1;
    for (float &x : b) x = amplitude * enaga_bipolar(enaga_xorshift(&state));
    return b;
}
// Seconds to frames.
inline size_t frames(double seconds) { return size_t(seconds * rate + 0.5); }

// ---- Running code in blocks, as audio() does -------------------------------------------------------------------

// `frames` frames from f(left, right, n), called a block of `block` frames at a time (the last may be shorter),
// each block's buffers cleared first: stereo interleaved out. For an engine's render or render_add.
template <class F> Buffer render(size_t frames, F f, int block = ENAGA_BLOCK_FRAMES) {
    Buffer out(2 * frames);
    std::vector<float> l(static_cast<size_t>(block)), r(static_cast<size_t>(block));
    const size_t step = static_cast<size_t>(block);
    for (size_t at = 0; at < frames; at += step) {
        int n = int(frames - at < step ? frames - at : step);
        std::fill(l.begin(), l.end(), 0.0f);
        std::fill(r.begin(), r.end(), 0.0f);
        f(l.data(), r.data(), n);
        for (int i = 0; i < n; i++) {
            out[2 * (at + size_t(i))] = l[size_t(i)];
            out[2 * (at + size_t(i)) + 1] = r[size_t(i)];
        }
    }
    return out;
}
// Runs f(buffer, n) on `in` a block at a time, in place, and returns the result: for a mono effect's process.
template <class F> Buffer process(Buffer in, F f, int block = ENAGA_BLOCK_FRAMES) {
    const size_t step = static_cast<size_t>(block);
    for (size_t at = 0; at < in.size(); at += step) {
        int n = int(in.size() - at < step ? in.size() - at : step);
        f(in.data() + at, n);
    }
    return in;
}

// One channel of interleaved stereo (0 left, 1 right); and two channels made into interleaved stereo.
inline Buffer channel(const Buffer &stereo, int which) {
    Buffer b(stereo.size() / 2);
    for (size_t i = 0; i < b.size(); i++) b[i] = stereo[2 * i + size_t(which)];
    return b;
}
inline Buffer interleave(const Buffer &left, const Buffer &right) {
    Buffer b(2 * std::min(left.size(), right.size()));
    for (size_t i = 0; i < b.size() / 2; i++) { b[2 * i] = left[i]; b[2 * i + 1] = right[i]; }
    return b;
}
// Frames from..to (to excluded) of a mono buffer.
inline Buffer slice(const Buffer &b, size_t from, size_t to) {
    to = std::min(to, b.size());
    return from < to ? Buffer(b.begin() + long(from), b.begin() + long(to)) : Buffer();
}

// ---- Measurements -----------------------------------------------------------------------------------------------

inline double peak(const Buffer &b) {
    double p = 0;
    for (float x : b) p = std::fmax(p, std::fabs(double(x)));
    return p;
}
inline double rms(const Buffer &b) {
    double s = 0;
    for (float x : b) s += double(x) * double(x);
    return b.empty() ? 0.0 : std::sqrt(s / double(b.size()));
}
inline double mean(const Buffer &b) {
    double s = 0;
    for (float x : b) s += x;
    return b.empty() ? 0.0 : s / double(b.size());
}
// An amplitude ratio in decibels (0 for silence is -inf).
inline double db(double ratio) { return 20.0 * std::log10(ratio); }
// The frequency of a mono signal from its rising zero crossings, interpolated: for a steady tone.
inline double frequency(const Buffer &b) {
    double first = -1, last = -1;
    int count = 0;
    for (size_t i = 1; i < b.size(); i++)
        if (b[i - 1] < 0 && b[i] >= 0) {
            double at = double(i - 1) + double(b[i - 1]) / double(b[i - 1] - b[i]);
            if (first < 0) first = at;
            last = at;
            count++;
        }
    return count < 2 ? 0.0 : (count - 1) * double(rate) / (last - first);
}
// The first frame whose magnitude reaches `threshold`, or -1: when a sound starts, a delay's echo arrives.
inline long first_above(const Buffer &b, double threshold) {
    for (size_t i = 0; i < b.size(); i++)
        if (std::fabs(double(b[i])) >= threshold) return long(i);
    return -1;
}
// The gain of a mono process at `hz`: a sine through f(buffer, n) (as process() calls it) for a second, the RMS
// of its last half over the input's. For a filter's response.
template <class F> double gain_at(double hz, F f) {
    Buffer in = sine(float(hz), size_t(rate)), out = process(in, f);
    return rms(slice(out, out.size() / 2, out.size())) / rms(slice(in, in.size() / 2, in.size()));
}

// ---- Checks -----------------------------------------------------------------------------------------------------

// Named checks: each failure is printed as it happens, and finish() prints the count and returns the exit status
// (return t.finish() from main), which host_test.py reports.
class Checks {
public:
    bool check(const char *what, bool ok) {
        count_++;
        if (!ok) {
            std::printf("FAIL %s\n", what);
            failed_++;
        }
        return ok;
    }
    bool near(const char *what, double got, double want, double tolerance) {
        bool ok = std::fabs(got - want) <= tolerance;
        if (!ok) std::printf("FAIL %s: got %.9g, want %.9g (within %g)\n", what, got, want, tolerance);
        return tally(ok);
    }
    bool below(const char *what, double got, double limit) {
        bool ok = got < limit;
        if (!ok) std::printf("FAIL %s: got %.9g, want below %.9g\n", what, got, limit);
        return tally(ok);
    }
    bool above(const char *what, double got, double limit) {
        bool ok = got > limit;
        if (!ok) std::printf("FAIL %s: got %.9g, want above %.9g\n", what, got, limit);
        return tally(ok);
    }
    // The same samples, bit for bit (block sizes, a C loop against the C++ class, a refactor).
    bool same(const char *what, const Buffer &a, const Buffer &b) {
        size_t i = 0;
        while (i < a.size() && i < b.size() && a[i] == b[i]) i++;
        bool ok = i == a.size() && i == b.size();
        if (!ok) std::printf("FAIL %s: differ from sample %zu of %zu / %zu\n", what, i, a.size(), b.size());
        return tally(ok);
    }
    int failed() const { return failed_; }
    int finish() const {
        if (failed_) std::printf("%d of %d checks failed\n", failed_, count_);
        else std::printf("ok: %d checks\n", count_);
        return failed_ ? 1 : 0;
    }

private:
    bool tally(bool ok) {
        count_++;
        failed_ += !ok;
        return ok;
    }
    int count_ = 0, failed_ = 0;
};

// ---- WAV files --------------------------------------------------------------------------------------------------

// Writes `samples` (interleaved if channels is 2) as a 32-bit float WAV at `sample_rate`; false if it could not.
inline bool write_wav(const std::string &path, const Buffer &samples, int channels = 2,
                      uint32_t sample_rate = ENAGA_SAMPLE_RATE) {
    wav::Format format;
    format.encoding = wav::floating;
    format.channels = uint16_t(channels);
    format.rate = sample_rate;
    format.bits = 32;
    uint8_t head[44];
    wav::header(head, format, uint32_t(samples.size() * 4));
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(head, 1, 44, f) == 44 &&
              std::fwrite(samples.data(), 4, samples.size(), f) == samples.size();
    return std::fclose(f) == 0 && ok;
}
// Reads a WAV file (any format Enaga reads: 8-, 16-, 24- and 32-bit PCM, 32-bit float) into float samples,
// interleaved if stereo; its channels and rate into the arguments. False if it could not.
inline bool read_wav(const std::string &path, Buffer &samples, int &channels, uint32_t &sample_rate) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> bytes;
    uint8_t chunk[65536];
    for (size_t got; (got = std::fread(chunk, 1, sizeof chunk, f)) > 0;) bytes.insert(bytes.end(), chunk, chunk + got);
    std::fclose(f);
    wav::Format format;
    if (!wav::parse(bytes.data(), uint32_t(bytes.size()), format)) return false;
    int pcm = enaga_pcm_format(format.encoding, format.bits);
    if (pcm < 0 || format.channels < 1) return false;
    uint32_t available = uint32_t(bytes.size()) - format.data_offset;
    uint32_t data = format.data_bytes && format.data_bytes < available ? format.data_bytes : available;
    size_t count = data / (enaga_pcm_bytes(pcm)) / format.channels * format.channels;
    samples.assign(count, 0.0f);
    enaga_pcm_to_float(bytes.data() + format.data_offset, pcm, 1, samples.data(), int(count));
    channels = format.channels;
    sample_rate = format.rate;
    return true;
}

// ---- The stand-in host (runtime/host/host.cpp) ------------------------------------------------------------------

// Lines the code logged (enaga::log) and MIDI messages it sent (<enaga/midi.hpp>), since the start or clear_host().
const std::vector<std::string> &logged();
const std::vector<std::vector<uint8_t>> &midi_sent();
void clear_host();
// What enaga::ms() returns.
void set_ms(uint32_t ms);
// Runs the clock at `bpm` from `from_tick`: each enaga::clock::ticks call (once per audio() block, as apps make
// it) takes the next block's ticks, at ENAGA_CLOCK_PPQN to the quarter note. stop_clock() stops it.
void run_clock(float bpm, uint32_t from_tick = 0);
void stop_clock();

}  // namespace enaga::testing
