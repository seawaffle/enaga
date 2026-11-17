// Enaga DSP: fast float math without libm. Provisional until 1.0.
//
// Accuracy is what audio needs, stated per function and checked by tests/test_dsp.py. Nothing here divides, so
// all of it is cheap enough for the audio callback; the C674x has no hardware divide and float division goes
// through a libgcc routine.
#pragma once

#include <bit>
#include <cstdint>

namespace enaga::dsp {

inline constexpr float pi = 3.14159265358979f;

inline float clamp(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

// Largest integer not above x, for |x| < 2^31.
inline int floor_int(float x) {
    int i = int(x);
    return float(i) > x ? i - 1 : i;
}

// 2^x, relative error under 3e-7 for -126 <= x <= 126 (clamped to that range).
inline float exp2(float x) {
    x = clamp(x, -126.0f, 126.0f);
    int i = floor_int(x);
    float f = x - float(i);
    float p = 1.0f + f * (0.69315308f + f * (0.24015361f + f * (0.05582631f + f * (0.00898934f + f * 0.00187757f))));
    return std::bit_cast<float>(std::bit_cast<uint32_t>(p) + (uint32_t(i) << 23));
}

inline float exp(float x) { return exp2(x * 1.44269504f); }

// The frequency ratio of a pitch offset in semitones.
inline float semitones(float s) { return exp2(s * (1.0f / 12.0f)); }

// log2 x for x > 0, absolute error under 1e-4.
inline float log2(float x) {
    uint32_t bits = std::bit_cast<uint32_t>(x);
    float e = float(int((bits >> 23) & 0xFF) - 127);
    float m = std::bit_cast<float>((bits & 0x007FFFFFu) | 0x3F800000u) - 1.0f;   // [0, 1)
    return e + m * (1.44254843f + m * (-0.71814525f + m * (0.45754091f + m * (-0.27790386f + m * (0.12179571f + m * -0.02584144f)))));
}

// sin(2 pi t) for any t (in turns), absolute error under 4e-6.
inline float sin_turns(float t) {
    t -= float(floor_int(t + 0.5f));           // [-0.5, 0.5)
    if (t > 0.25f) t = 0.5f - t;
    else if (t < -0.25f) t = -0.5f - t;        // [-0.25, 0.25]: sin is odd and symmetric about a quarter turn
    float t2 = t * t;
    return t * (6.28318531f + t2 * (-41.3417022f + t2 * (81.6052493f + t2 * (-76.7058597f + t2 * 42.0586939f))));
}

inline float cos_turns(float t) { return sin_turns(t + 0.25f); }

// tan(pi x) for 0 <= x <= 0.49, relative error under 1e-4: the prewarped cutoff of a TPT filter, x = cutoff /
// sample rate. One division.
inline float tan_pi(float x) {
    x = clamp(x, 0.0f, 0.49f);
    return sin_turns(0.5f * x) / cos_turns(0.5f * x);
}

// A soft clip: x for small x, smoothly to +-1 at |x| >= 1.5, cubic in between.
inline float soft_clip(float x) {
    x = clamp(x, -1.5f, 1.5f);
    return x - x * x * x * (4.0f / 27.0f);
}

// A fast uniform random source (xorshift32).
class Random {
public:
    explicit Random(uint32_t seed = 0x2545F491u) : state_(seed ? seed : 1) {}
    uint32_t next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return state_;
    }
    float bipolar() { return float(int32_t(next())) * (1.0f / 2147483648.0f); }   // [-1, 1)
    float unipolar() { return float(next() >> 8) * (1.0f / 16777216.0f); }        // [0, 1)

private:
    uint32_t state_;
};

}  // namespace enaga::dsp
