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
    DelayLine(const DelayLine &) = delete;
    DelayLine &operator=(const DelayLine &) = delete;
    ~DelayLine() { delete[] buffer_; }

    void allocate(uint32_t max_samples) {
        uint32_t size = 1;
        while (size < max_samples + 2) size <<= 1;
        delete[] buffer_;
        buffer_ = new float[size]();
        mask_ = size - 1;
        write_ = 0;
    }
    uint32_t capacity() const { return mask_ - 1; }

    void write(float x) { buffer_[write_ = (write_ + 1) & mask_] = x; }
    float tap(uint32_t delay) const { return buffer_[(write_ - delay) & mask_]; }   // 0 = the last write
    float tap_linear(float delay) const {       // fractional delay, 0 to capacity()
        int whole = int(delay);
        float frac = delay - float(whole);
        return lerp(tap(uint32_t(whole)), tap(uint32_t(whole) + 1), frac);
    }

private:
    float *buffer_ = nullptr;
    uint32_t mask_ = 0, write_ = 0;
};

// A stereo delay: time, feedback through a low-pass (tone), and an optional ping-pong between the channels.
// The delay time glides to new settings, so changing it bends the pitch like tape instead of clicking.
class StereoDelay {
public:
    void init(float max_seconds, float sample_rate) {
        rate_ = sample_rate;
        left_.allocate(uint32_t(max_seconds * sample_rate) + 4);
        right_.allocate(uint32_t(max_seconds * sample_rate) + 4);
        current_ = target_ = 0.25f * sample_rate;
    }
    // seconds; feedback 0-0.95; tone 0 (dark) to 1 (bright); ping_pong crosses the feedback.
    void set(float seconds, float feedback, float tone, bool ping_pong) {
        target_ = clamp(seconds * rate_, 16.0f, float(left_.capacity() - 2));
        feedback_ = clamp(feedback, 0.0f, 0.95f);
        tone_ = OnePole::coefficient(lerp(800.0f, 16000.0f, clamp(tone, 0.0f, 1.0f) * clamp(tone, 0.0f, 1.0f)), rate_);
        ping_pong_ = ping_pong;
    }
    // Adds mix x the wet signal to l and r; frames of in_l / in_r feed it.
    void process(const float *in_l, const float *in_r, float *out_l, float *out_r, int n, float mix) {
        for (int i = 0; i < n; i++) {
            current_ += 0.0005f * (target_ - current_);
            float dl = left_.tap_linear(current_), dr = right_.tap_linear(current_);
            float fl = damp_l_.step(dl, tone_), fr = damp_r_.step(dr, tone_);
            if (ping_pong_) {
                left_.write(0.5f * (in_l[i] + in_r[i]) + feedback_ * fr);
                right_.write(feedback_ * fl);
            } else {
                left_.write(in_l[i] + feedback_ * fl);
                right_.write(in_r[i] + feedback_ * fr);
            }
            out_l[i] += mix * dl;
            out_r[i] += mix * dr;
        }
    }

private:
    DelayLine left_, right_;
    OnePole damp_l_, damp_r_;
    float rate_ = 48000.0f, current_ = 0.0f, target_ = 0.0f, feedback_ = 0.0f, tone_ = 1.0f;
    bool ping_pong_ = false;
};

// A reverb: two input diffusers into an eight-line feedback delay network (Hadamard mixing), each line damped
// by a low-pass and scaled for the decay time. The network's lengths are fixed; DECAY sets how long it rings.
class Reverb {
public:
    static constexpr int kLines = 8;

    void init(float sample_rate) {
        rate_ = sample_rate;
        float scale = sample_rate / 48000.0f;
        for (int i = 0; i < kLines; i++) {
            length_[i] = uint32_t(float(kLengths[i]) * scale);
            lines_[i].allocate(length_[i]);
        }
        for (int i = 0; i < 4; i++) {
            diffuse_length_[i] = uint32_t(float(kDiffuse[i]) * scale);
            diffusers_[i].allocate(diffuse_length_[i]);
        }
        set(2.0f, 0.5f);
    }
    // decay_s: time to fall 60 dB (0.2-30); damping 0 (bright) to 1 (dark).
    void set(float decay_s, float damping) {
        decay_s = clamp(decay_s, 0.2f, 30.0f);
        for (int i = 0; i < kLines; i++)       // 10^(-3 length / (T60 rate)) = 2^(-9.966 length / (T60 rate))
            gain_[i] = exp2(-9.96578428f * float(length_[i]) / (decay_s * rate_));
        damping_ = OnePole::coefficient(lerp(14000.0f, 1500.0f, clamp(damping, 0.0f, 1.0f)), rate_);
    }
    void process(const float *in_l, const float *in_r, float *out_l, float *out_r, int n, float mix) {
        for (int i = 0; i < n; i++) {
            float x = 0.25f * (in_l[i] + in_r[i]);
            for (int d = 0; d < 4; d++) {      // Schroeder all-passes, g = 0.6
                float delayed = diffusers_[d].tap(diffuse_length_[d] - 1);
                float v = x + 0.6f * delayed;
                diffusers_[d].write(v);
                x = delayed - 0.6f * v;
            }
            float s[kLines];
            for (int j = 0; j < kLines; j++) s[j] = damp_[j].step(lines_[j].tap(length_[j] - 1), damping_) * gain_[j];
            hadamard(s);
            for (int j = 0; j < kLines; j++) lines_[j].write(s[j] + x);
            float l = s[0] + s[2] + s[4] + s[6], r = s[1] + s[3] + s[5] + s[7];
            out_l[i] += mix * l;
            out_r[i] += mix * r;
        }
    }

private:
    static void hadamard(float *s) {           // the 8-point Hadamard transform, scaled to keep energy
        for (int span = 1; span < kLines; span <<= 1)
            for (int j = 0; j < kLines; j += span << 1)
                for (int k = j; k < j + span; k++) {
                    float a = s[k], b = s[k + span];
                    s[k] = a + b;
                    s[k + span] = a - b;
                }
        for (int j = 0; j < kLines; j++) s[j] *= 0.35355339f;   // 1 / sqrt(8)
    }

    static constexpr uint32_t kLengths[kLines] = {1123, 1361, 1597, 1823, 2083, 2357, 2689, 3037};   // primes
    static constexpr uint32_t kDiffuse[4] = {149, 211, 293, 397};
    DelayLine lines_[kLines], diffusers_[4];
    OnePole damp_[kLines];
    uint32_t length_[kLines] = {}, diffuse_length_[4] = {};
    float gain_[kLines] = {}, damping_ = 1.0f, rate_ = 48000.0f;
};

}  // namespace enaga::dsp
