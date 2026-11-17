/* Enaga DSP: fast float math without libm, for C and C++ (math.hpp wraps it as enaga::dsp). Provisional until 1.0.
 *
 * Accuracy is what audio needs, stated per function and checked by tests/test_dsp.py. Nothing here divides
 * (the C674x has no hardware divide: a float division is a run of code, and under cl6x a call, which stops a loop
 * from pipelining); enaga_reciprocal is 1 / x without one. Everything is written
 * with selects rather than branches, so loops that use it can be software-pipelined by cl6x (*.ti.c files). */
#ifndef ENAGA_DSP_MATH_H
#define ENAGA_DSP_MATH_H

#include <stdint.h>

#define ENAGA_PI 3.14159265358979f

static inline float enaga_clamp(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
static inline float enaga_lerp(float a, float b, float t) { return a + (b - a) * t; }

/* Largest integer not above x, for |x| < 2^31. */
static inline int enaga_floor_int(float x) {
    int i = (int)x;
    return (float)i > x ? i - 1 : i;
}

static inline uint32_t enaga_float_bits(float x) {
    union { float f; uint32_t u; } v;
    v.f = x;
    return v.u;
}
static inline float enaga_bits_float(uint32_t u) {
    union { float f; uint32_t u; } v;
    v.u = u;
    return v.f;
}

/* 1 / x for 1e-37 < |x| < 1e37, relative error under 2e-7: three Newton steps from a guess made of the bits. */
static inline float enaga_reciprocal(float x) {
    float r = enaga_bits_float(0x7EF311C3u - enaga_float_bits(x));
    r = r * (2.0f - x * r);
    r = r * (2.0f - x * r);
    return r * (2.0f - x * r);
}

/* 2^x, relative error under 3e-7 for -126 <= x <= 126 (clamped to that range). */
static inline float enaga_exp2(float x) {
    x = enaga_clamp(x, -126.0f, 126.0f);
    int i = enaga_floor_int(x);
    float f = x - (float)i;
    float p = 1.0f + f * (0.69315308f + f * (0.24015361f + f * (0.05582631f + f * (0.00898934f + f * 0.00187757f))));
    return enaga_bits_float(enaga_float_bits(p) + ((uint32_t)i << 23));
}

static inline float enaga_exp(float x) { return enaga_exp2(x * 1.44269504f); }

/* The frequency ratio of a pitch offset in semitones. */
static inline float enaga_semitones(float s) { return enaga_exp2(s * (1.0f / 12.0f)); }

/* log2 x for x > 0, absolute error under 1e-4. */
static inline float enaga_log2(float x) {
    uint32_t bits = enaga_float_bits(x);
    float e = (float)((int)((bits >> 23) & 0xFF) - 127);
    float m = enaga_bits_float((bits & 0x007FFFFFu) | 0x3F800000u) - 1.0f;   /* [0, 1) */
    return e + m * (1.44254843f + m * (-0.71814525f + m * (0.45754091f + m * (-0.27790386f + m * (0.12179571f + m * -0.02584144f)))));
}

/* sin(2 pi t) for any t (in turns), absolute error under 4e-6. */
static inline float enaga_sin_turns(float t) {
    t -= (float)enaga_floor_int(t + 0.5f);                          /* [-0.5, 0.5) */
    t = t > 0.25f ? 0.5f - t : t < -0.25f ? -0.5f - t : t;          /* [-0.25, 0.25]: odd, symmetric about 1/4 */
    float t2 = t * t;
    return t * (6.28318531f + t2 * (-41.3417022f + t2 * (81.6052493f + t2 * (-76.7058597f + t2 * 42.0586939f))));
}

static inline float enaga_cos_turns(float t) { return enaga_sin_turns(t + 0.25f); }

/* tan(pi x) for 0 <= x <= 0.49, relative error under 1e-4: the prewarped cutoff of a TPT filter, x = cutoff /
 * sample rate. */
static inline float enaga_tan_pi(float x) {
    x = enaga_clamp(x, 0.0f, 0.49f);
    return enaga_sin_turns(0.5f * x) * enaga_reciprocal(enaga_cos_turns(0.5f * x));
}

/* A soft clip: x for small x, smoothly to +-1 at |x| >= 1.5, cubic in between. */
static inline float enaga_soft_clip(float x) {
    x = enaga_clamp(x, -1.5f, 1.5f);
    return x - x * x * x * (4.0f / 27.0f);
}

/* xorshift32: the next value of a non-zero state, and that value as a float in [-1, 1) or [0, 1). */
static inline uint32_t enaga_xorshift(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *state = x;
}
static inline float enaga_bipolar(uint32_t x) { return (float)(int32_t)x * (1.0f / 2147483648.0f); }
static inline float enaga_unipolar(uint32_t x) { return (float)(x >> 8) * (1.0f / 16777216.0f); }

#endif
