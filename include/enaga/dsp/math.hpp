// Enaga DSP: fast float math without libm. Provisional until 1.0.
//
// The C++ names for math.h (enaga_exp2 is enaga::dsp::exp2, and so on), where the accuracy of each is stated.
// Block functions on buffers (enaga_mix, enaga_scale, ...) are in <enaga/dsp.h>, callable as they are.
// Nothing here divides except tan_pi; the C674x has no hardware divide and float division goes through a libgcc
// routine.
#pragma once

#include <cstdint>

#include <enaga/dsp.h>

namespace enaga::dsp {

inline constexpr float pi = ENAGA_PI;

inline float clamp(float x, float lo, float hi) { return enaga_clamp(x, lo, hi); }
inline float lerp(float a, float b, float t) { return enaga_lerp(a, b, t); }
inline int floor_int(float x) { return enaga_floor_int(x); }
inline float exp2(float x) { return enaga_exp2(x); }
inline float exp(float x) { return enaga_exp(x); }
inline float semitones(float s) { return enaga_semitones(s); }
inline float log2(float x) { return enaga_log2(x); }
inline float sin_turns(float t) { return enaga_sin_turns(t); }
inline float cos_turns(float t) { return enaga_cos_turns(t); }
inline float tan_pi(float x) { return enaga_tan_pi(x); }
inline float soft_clip(float x) { return enaga_soft_clip(x); }

// A fast uniform random source (xorshift32).
class Random {
public:
    explicit Random(uint32_t seed = 0x2545F491u) : state_(seed ? seed : 1) {}
    uint32_t next() { return enaga_xorshift(&state_); }
    float bipolar() { return enaga_bipolar(next()); }     // [-1, 1)
    float unipolar() { return enaga_unipolar(next()); }   // [0, 1)
    // Adds level x white noise (the next n bipolar values) to out[0..n): a block function (enaga_noise_add).
    void add_noise(float *out, int n, float level) { enaga_noise_add(&state_, out, n, level); }
    uint32_t *state() { return &state_; }    // for C code (enaga_xorshift) working on the same stream

private:
    uint32_t state_;
};

}  // namespace enaga::dsp
