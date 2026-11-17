// Enaga DSP: delay lines, a stereo delay and a reverb. Provisional until 1.0.
//
// Buffers come from the app heap: construct these in init() (or as members of an object made there), not in
// the audio callback. Lengths are rounded up to a power of two so reads wrap with a mask.
#pragma once

#include <enaga/dsp/filter.hpp>
#include <enaga/dsp/math.hpp>

namespace enaga::dsp {

class DelayLine {
public:
    DelayLine() = default;
    explicit DelayLine(uint32_t max_samples) { allocate(max_samples); }

    void allocate(uint32_t max_samples) {
        uint32_t size = enaga_delay_size(max_samples);
        enaga_delay_init(&state_, memory_.allocate(size), size);
    }
    uint32_t capacity() const { return enaga_delay_capacity(&state_); }

    void write(float x) { enaga_delay_write(&state_, x); }
    float tap(uint32_t delay) const { return enaga_delay_tap(&state_, delay); }   // 0 = the last write
    float tap_linear(float delay) const { return enaga_delay_tap_linear(&state_, delay); }   // 0 to capacity()
    // Blocks: read n taps as samples 0..n-1 of a block whose writes follow (delay >= n - 1), then write the block.
    void read(uint32_t delay, float *out, int n) const { enaga_delay_read(&state_, delay, out, n); }
    void write(const float *in, int n) { enaga_delay_write_block(&state_, in, n); }
    // A modulated block read: out[i] = tap_linear(delay[i]) at sample i, every delay[i] >= i.
    void read_linear(const float *delay, float *out, int n) const { enaga_delay_read_linear(&state_, delay, out, n); }

    enaga_delay_line state_{};     // public, for C code working on the same line

private:
    Memory memory_;
};

// A stereo delay: time, feedback through a low-pass (tone), and an optional ping-pong between the channels.
// The delay time glides to new settings, so changing it bends the pitch like tape instead of clicking.
// enaga_stereo_delay_*: block functions.
class StereoDelay {
public:
    void init(float max_seconds, float sample_rate) {
        enaga_stereo_delay_init(&state_, memory_.allocate(enaga_stereo_delay_memory(max_seconds, sample_rate)),
                                max_seconds, sample_rate);
    }
    // seconds; feedback 0-0.95; tone 0 (dark) to 1 (bright); ping_pong crosses the feedback.
    void set(float seconds, float feedback, float tone, bool ping_pong) {
        enaga_stereo_delay_set(&state_, seconds, feedback, tone, ping_pong);
    }
    // Adds mix x the wet signal to out_l / out_r; frames of in_l / in_r feed it.
    void process(const float *in_l, const float *in_r, float *out_l, float *out_r, int n, float mix) {
        enaga_stereo_delay_process(&state_, in_l, in_r, out_l, out_r, n, mix);
    }

    enaga_stereo_delay state_{};

private:
    Memory memory_;
};

// A reverb: four Schroeder all-passes into an eight-line feedback delay network (Hadamard mixing), each line
// damped by a low-pass and scaled for the decay time. The network's lengths are fixed; DECAY sets how long it
// rings. enaga_reverb_*: block functions.
class Reverb {
public:
    static constexpr int kLines = ENAGA_REVERB_LINES;

    void init(float sample_rate) {
        enaga_reverb_init(&state_, memory_.allocate(enaga_reverb_memory(sample_rate)), sample_rate);
    }
    // decay_s: time to fall 60 dB (0.2-30); damping 0 (bright) to 1 (dark).
    void set(float decay_s, float damping) { enaga_reverb_set(&state_, decay_s, damping); }
    // Adds mix x the reverb of in_l + in_r to out_l / out_r.
    void process(const float *in_l, const float *in_r, float *out_l, float *out_r, int n, float mix) {
        enaga_reverb_process(&state_, in_l, in_r, out_l, out_r, n, mix);
    }

    enaga_reverb state_{};

private:
    Memory memory_;
};

}  // namespace enaga::dsp
