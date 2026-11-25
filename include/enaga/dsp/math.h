/* Enaga DSP: fast float math, for C and C++ (math.hpp wraps it as enaga::dsp). Provisional until 1.0. Faster and
 * less exact than the standard functions (<math.h>, runtime/libm.c), which are full float accuracy.
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

/* log2 x for x > 0: absolute error under 2e-7 for 0.5 <= x <= 2, and within 2e-7 x |log2 x| elsewhere (the
 * float rounding of the result). The mantissa is taken to [sqrt(1/2), sqrt(2)), then a degree-8 polynomial. */
static inline float enaga_log2(float x) {
    uint32_t bits = enaga_float_bits(x);
    uint32_t high = (bits & 0x007FFFFFu) > 0x003504F3u;                /* mantissa above sqrt(2): halve it */
    float e = (float)((int)((bits >> 23) & 0xFF) - 127 + (int)high);
    float u = enaga_bits_float((bits & 0x007FFFFFu) | (0x3F800000u - (high << 23))) - 1.0f;
    return e + u * (1.44269478f + u * (-0.72135717f + u * (0.48093945f + u * (-0.36008719f + u * (0.28670755f +
           u * (-0.25006929f + u * (0.23688978f + u * -0.14574295f)))))));
}

static inline float enaga_log(float x) { return enaga_log2(x) * 0.693147181f; }       /* natural log, x > 0 */
static inline float enaga_log10(float x) { return enaga_log2(x) * 0.301029996f; }     /* x > 0 */

/* x^y for x > 0 (0 for x <= 0): exp2(y log2 x), relative error about 3e-7 + 1.4e-7 |y log2 x|. */
static inline float enaga_pow(float x, float y) {
    return x > 0.0f ? enaga_exp2(y * enaga_log2(x)) : 0.0f;
}

/* Decibels to an amplitude ratio (0 dB is 1, -6.02 dB is 0.5), and back: gain_to_db floors at -600 dB, which is
 * what 0 and anything below 1e-30 give. Accuracy as exp2 and log2. */
static inline float enaga_db_to_gain(float db) { return enaga_exp2(db * 0.166096405f); }
static inline float enaga_gain_to_db(float gain) {
    return 6.02059991f * enaga_log2(gain > 1e-30f ? gain : 1e-30f);
}

/* |x|, x with the sign of s, and x's fraction x - floor(x) in [0, 1) (|x| < 2^31): wrapping a phase. */
static inline float enaga_abs(float x) { return enaga_bits_float(enaga_float_bits(x) & 0x7FFFFFFFu); }
static inline float enaga_copysign(float x, float s) {
    return enaga_bits_float((enaga_float_bits(x) & 0x7FFFFFFFu) | (enaga_float_bits(s) & 0x80000000u));
}
static inline float enaga_wrap(float x) {
    float r = x - (float)enaga_floor_int(x);
    return r < 1.0f ? r : 0.0f;                                     /* a tiny negative x rounds up to 1 */
}

/* 1 / sqrt(x) for 1e-37 < x < 1e37, and sqrt(x) for 0 <= x < 1e37 (0 for x < 0): relative error under 3e-7,
 * three Newton steps from a guess made of the bits. */
static inline float enaga_rsqrt(float x) {
    float r = enaga_bits_float(0x5F375A86u - (enaga_float_bits(x) >> 1)), h = 0.5f * x;
    r = r * (1.5f - h * r * r);
    r = r * (1.5f - h * r * r);
    return r * (1.5f - h * r * r);
}
static inline float enaga_sqrt(float x) {
    x = x > 0.0f ? x : 0.0f;
    return x * enaga_rsqrt(x);
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

/* sin, cos and tan of x in radians, through the turn functions: absolute error under 4e-6 + 1e-7 |x| (sin, cos);
 * tan is sin / cos, relative error about 1e-5 away from its poles. */
static inline float enaga_sin(float x) { return enaga_sin_turns(x * 0.159154943f); }
static inline float enaga_cos(float x) { return enaga_cos_turns(x * 0.159154943f); }
static inline float enaga_tan(float x) {
    float t = x * 0.159154943f;
    return enaga_sin_turns(t) * enaga_reciprocal(enaga_cos_turns(t));
}

/* atan on [0, 1]: a degree-15 odd polynomial, absolute error under 2e-7. */
static inline float enaga_atan_unit(float t) {
    float t2 = t * t;
    return t * (0.99999994f + t2 * (-0.33332095f + t2 * (0.19971372f + t2 * (-0.14029400f + t2 * (0.09942708f +
           t2 * (-0.05990401f + t2 * (0.02455663f + t2 * -0.00478032f)))))));
}

/* atan(x), and atan2(y, x), the angle of the point (x, y) in (-pi, pi] (0 for the origin): absolute error under
 * 5e-7. asin and acos of x in [-1, 1] (clamped): absolute error under 1e-6, and 4e-4 within 1e-6 of +-1. */
static inline float enaga_atan(float x) {
    float a = enaga_abs(x), big = a > 1.0f ? 1.0f : 0.0f;
    float t = big != 0.0f ? enaga_reciprocal(a > 1.0f ? a : 1.0f) : a;
    float r = enaga_atan_unit(t);
    return enaga_copysign(big != 0.0f ? 1.57079633f - r : r, x);
}
static inline float enaga_atan2(float y, float x) {
    float ax = enaga_abs(x), ay = enaga_abs(y);
    float hi = ax > ay ? ax : ay, lo = ax > ay ? ay : ax;
    float r = enaga_atan_unit(lo * enaga_reciprocal(hi > 1e-37f ? hi : 1e-37f));
    r = ay > ax ? 1.57079633f - r : r;
    r = x < 0.0f ? 3.14159265f - r : r;
    return enaga_copysign(r, y);
}
static inline float enaga_asin(float x) {
    x = enaga_clamp(x, -1.0f, 1.0f);
    return enaga_atan2(x, enaga_sqrt((1.0f - x) * (1.0f + x)));
}
static inline float enaga_acos(float x) {
    x = enaga_clamp(x, -1.0f, 1.0f);
    return enaga_atan2(enaga_sqrt((1.0f - x) * (1.0f + x)), x);
}

/* tanh(x), the smooth saturation curve: relative error under 6e-7. An odd polynomial for |x| < 0.55, else
 * 1 - 2 / (e^2|x| + 1); exactly +-1 from |x| = 9.1. */
static inline float enaga_tanh(float x) {
    float a = enaga_clamp(enaga_abs(x), 0.0f, 10.0f), a2 = a * a;
    float small = a * (0.99999994f + a2 * (-0.33332810f + a2 * (0.13319130f + a2 * (-0.05260273f + a2 * 0.01633759f))));
    float large = 1.0f - 2.0f * enaga_reciprocal(enaga_exp2(a * 2.88539008f) + 1.0f);
    return enaga_copysign(a < 0.55f ? small : large, x);
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
