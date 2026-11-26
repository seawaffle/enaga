/* Enaga: the standard float math functions for apps (<math.h> and <cmath>, include/libm), so code written against
 * libm builds unchanged. Built into the DSP library's archive (tools/build_app.py), by cl6x when installed: an app
 * links only the functions it calls.
 *
 * Each works in double internally (the C674x adds, multiplies and converts doubles in hardware), so results are
 * within 1 ulp of the exact value, mostly correctly rounded (tests/dsp/host.cpp measures every function against the
 * host's libm). That costs a few hundred cycles a call: for inner loops, <enaga/dsp/math.h>'s functions are several
 * times faster at slightly lower accuracy. Nothing divides at run time (the C674x has no divide instruction, and
 * cl6x's division needs TI's run-time library): 1 / x is enaga_reciprocal's guess refined in double.
 *
 * Only float functions: an app gets no double math (sinf, not sin), as cmath's deleted double overloads say.
 * sinf, cosf and tanf reduce their argument exactly at any size (Payne and Hanek's method past 1.6e6).
 *
 * ENAGA_LIBM_HOST renames everything enaga_host_<name>, for the host test, whose own libm has the real names. */
#include <stdint.h>

#include <enaga/dsp/math.h>

#ifdef ENAGA_LIBM_HOST
#define F(name) enaga_host_##name
#else
#define F(name) name
#endif

/* ---- Doubles by their words (little-endian: word 1 holds the sign, exponent and top of the mantissa) ---- */

typedef union { double d; uint32_t w[2]; } dwords;

static double from_words(uint32_t high, uint32_t low) {
    dwords v;
    v.w[0] = low;
    v.w[1] = high;
    return v.d;
}
static uint32_t high_word(double x) {
    dwords v;
    v.d = x;
    return v.w[1];
}
static double with_high(double x, uint32_t high) {
    dwords v;
    v.d = x;
    v.w[1] = high;
    return v.d;
}

/* 2^n for -1022 <= n <= 1023. */
static double power2(int n) { return from_words((uint32_t)(n + 1023) << 20, 0); }

static int is_nan(float x) { return x != x; }
static int is_inf(float x) { return (enaga_float_bits(x) & 0x7FFFFFFFu) == 0x7F800000u; }
static float nan_value(void) { return enaga_bits_float(0x7FC00000u); }
static float inf_value(void) { return enaga_bits_float(0x7F800000u); }

/* The nearest integer to x, |x| < 2^30. */
static int nearest(double x) { return (int)(x + (x < 0 ? -0.5 : 0.5)); }

/* 1 / x for a finite non-zero normal double: x = m 2^e with m in [1, 2), the float guess for 1 / m, two Newton
 * steps in double, then 2^-e. */
static double recip_d(double x) {
    uint32_t high = high_word(x);
    int e = (int)((high >> 20) & 0x7FF) - 1023;
    double m = with_high(x, (high & 0x800FFFFFu) | 0x3FF00000u), r;
    r = (double)enaga_reciprocal((float)m);
    r = r * (2.0 - m * r);
    r = r * (2.0 - m * r);
    return e > 1000 ? r * power2(-1000) * power2(1000 - e) : r * power2(-e);
}

/* sqrt of a finite x >= 0 of any double size: the scale taken out by the exponent, then 1 / sqrt by the float
 * guess and three Newton steps in double. */
static double sqrt_d(double x) {
    uint32_t high = high_word(x);
    int e = (int)((high >> 20) & 0x7FF) - 1023, half;
    double m, r;
    if (x == 0.0) return 0.0;
    if (e == -1023) {                                                   /* subnormal double: scale up first */
        x *= power2(600);
        high = high_word(x);
        e = (int)((high >> 20) & 0x7FF) - 1023 - 600;
    }
    half = e >> 1;                                                      /* floor(e / 2) */
    m = with_high(x, (high & 0x800FFFFFu) | (uint32_t)(e - 2 * half + 1023) << 20);   /* [1, 4) */
    r = (double)enaga_rsqrt((float)m);
    r = r * (1.5 - 0.5 * m * r * r);
    r = r * (1.5 - 0.5 * m * r * r);
    r = r * (1.5 - 0.5 * m * r * r);
    return m * r * power2(half);
}

/* 2^x for |x| <= 1000: 2^k times e^(f ln 2), f in [-0.5, 0.5], by its Taylor series (to 2e-13). */
static double exp2_d(double x) {
    int k = nearest(x);
    double f = (x - k) * 0.69314718055994530942, p;
    p = 1.0 + f * (1.0 + f * (0.5 + f * (1.6666666666666666e-1 + f * (4.1666666666666664e-2 + f * (8.3333333333333332e-3 +
        f * (1.3888888888888889e-3 + f * (1.9841269841269841e-4 + f * (2.4801587301587302e-5 + f * (2.7557319223985893e-6 +
        f * 2.7557319223985888e-7)))))))));
    if (k > 1000) return p * power2(1000) * power2(k - 1000);
    if (k < -1000) return p * power2(-1000) * power2(k + 1000);
    return p * power2(k);
}

static double exp_d(double x) { return exp2_d(x * 1.44269504088896340736); }

/* e^x - 1, exact near 0: the series for |x| < 0.5, else exp_d(x) - 1. */
static double expm1_d(double x) {
    if (x > -0.5 && x < 0.5)
        return x * (1.0 + x * (0.5 + x * (1.6666666666666666e-1 + x * (4.1666666666666664e-2 + x * (8.3333333333333332e-3 +
               x * (1.3888888888888889e-3 + x * (1.9841269841269841e-4 + x * (2.4801587301587302e-5 + x * (2.7557319223985893e-6 +
               x * (2.7557319223985888e-7 + x * (2.5052108385441720e-8 + x * (2.0876756987868100e-9 + x * 1.6059043836821613e-10))))))))))));
    return exp_d(x) - 1.0;
}

/* ln x for a finite x > 0: x = m 2^e with m in [sqrt(1/2), sqrt(2)), ln m = 2 atanh(s), s = (m - 1) / (m + 1),
 * |s| < 0.172, by the series (to 1e-14). */
static double log_d(double x) {
    uint32_t high;
    int e;
    double m, s, s2;
    if (high_word(x) < 0x00100000u) {                                   /* subnormal double */
        x *= power2(600);
        e = -600;
    } else {
        e = 0;
    }
    high = high_word(x);
    e += (int)((high >> 20) & 0x7FF) - 1023;
    m = with_high(x, (high & 0x000FFFFFu) | 0x3FF00000u);               /* [1, 2) */
    if (m > 1.41421356237309504880) {
        m *= 0.5;
        e++;
    }
    s = (m - 1.0) * recip_d(m + 1.0);
    s2 = s * s;
    return e * 0.69314718055994530942 +
           2.0 * s * (1.0 + s2 * (3.3333333333333333e-1 + s2 * (2.0e-1 + s2 * (1.4285714285714285e-1 + s2 * (1.1111111111111111e-1 +
           s2 * (9.0909090909090912e-2 + s2 * (7.6923076923076927e-2 + s2 * 6.6666666666666666e-2)))))));
}

/* sin and cos of |r| <= pi / 4, by their series (to 3e-14). */
static double sin_kernel(double r) {
    double r2 = r * r;
    return r * (1.0 + r2 * (-1.6666666666666666e-1 + r2 * (8.3333333333333332e-3 + r2 * (-1.9841269841269841e-4 +
           r2 * (2.7557319223985893e-6 + r2 * (-2.5052108385441720e-8 + r2 * 1.6059043836821613e-10))))));
}
static double cos_kernel(double r) {
    double r2 = r * r;
    return 1.0 + r2 * (-0.5 + r2 * (4.1666666666666664e-2 + r2 * (-1.3888888888888889e-3 + r2 * (2.4801587301587302e-5 +
           r2 * (-2.7557319223985888e-7 + r2 * (2.0876756987868100e-9 + r2 * -1.1470745597729725e-11))))));
}

/* The bits of 2 / pi from the binary point, preceded by 64 zero bits (bit positions -63 to 0). */
static const uint32_t two_over_pi[10] = {
    0, 0, 0xA2F9836Eu, 0x4E441529u, 0xFC2757D1u, 0xF534DDC0u, 0xDB629599u, 0x3C439041u, 0xFE5163ABu, 0xDEBBC561u,
};

/* 32 bits of 2 / pi starting at bit position pos (-63 <= pos <= 225; position 1 is the first after the point). */
static uint32_t two_over_pi_bits(int pos) {
    int index = (pos + 63) >> 5, shift = (pos + 63) & 31;
    return shift ? two_over_pi[index] << shift | two_over_pi[index + 1] >> (32 - shift) : two_over_pi[index];
}

/* x = k pi/2 + r, |r| <= pi/4, quadrant = k mod 4. Below 1.6e6, Cody and Waite's reduction with pi/2 in three
 * parts, the first two of 33 bits so their products with k are exact for |k| < 2^20. Beyond, Payne and Hanek's:
 * x = M 2^E with M a 24-bit integer, and x 2/pi mod 4 is M times the 96 bits of 2/pi from position E - 1 (the
 * bits before contribute multiples of 4), taken mod 2^96. */
static double reduce(float x, int *quadrant) {
    double xd = x;
    uint32_t bits = enaga_float_bits(x), m, w0, w1, w2, r0, r1, r2;
    uint64_t p;
    int e, k;
    double f;
    if ((bits & 0x7FFFFFFFu) < 0x49C80000u) {                          /* |x| < 1638400 */
        k = nearest(xd * 0.63661977236758134308);
        *quadrant = k & 3;
        return ((xd - k * 1.57079632673412561417e+00) - k * 6.07710050630396597660e-11) - k * 2.02226624879595063154e-21;
    }
    m = (bits & 0x007FFFFFu) | 0x00800000u;
    e = (int)((bits >> 23) & 0xFF) - 150;                               /* x = m 2^e, e >= -3 here */
    w0 = two_over_pi_bits(e - 1);
    w1 = two_over_pi_bits(e + 31);
    w2 = two_over_pi_bits(e + 63);
    p = (uint64_t)m * w2;
    r2 = (uint32_t)p;
    p = (uint64_t)m * w1 + (p >> 32);
    r1 = (uint32_t)p;
    r0 = (uint32_t)((uint64_t)m * w0 + (p >> 32));                      /* x 2/pi mod 4 = (r0 r1 r2) 2^-94 */
    k = (int)(r0 >> 30);
    f = (double)(r0 & 0x3FFFFFFFu) * 9.3132257461547852e-10 + (double)r1 * 2.1684043449710089e-19 +
        (double)r2 * 5.0487097934144756e-29;                           /* 2^-30, 2^-62, 2^-94: [0, 1) */
    if (f >= 0.5) {
        f -= 1.0;
        k++;
    }
    f *= 1.57079632679489661923;
    if (bits >> 31) {
        f = -f;
        k = -k;
    }
    *quadrant = k & 3;
    return f;
}

/* atan of 0 <= t <= 1: past tan(pi/12), atan t = pi/6 + atan((t sqrt 3 - 1) / (t + sqrt 3)), leaving |u| <= 0.268
 * for the series (to 4e-15). */
static double atan_unit(double t) {
    double base = 0.0, u2;
    if (t > 0.26794919243112270647) {
        t = (t * 1.73205080756887729353 - 1.0) * recip_d(t + 1.73205080756887729353);
        base = 0.52359877559829887308;
    }
    u2 = t * t;
    return base + t * (1.0 + u2 * (-3.3333333333333333e-1 + u2 * (2.0e-1 + u2 * (-1.4285714285714285e-1 + u2 * (1.1111111111111111e-1 +
           u2 * (-9.0909090909090912e-2 + u2 * (7.6923076923076927e-2 + u2 * (-6.6666666666666666e-2 + u2 * (5.8823529411764705e-2 +
           u2 * (-5.2631578947368418e-2 + u2 * 4.7619047619047616e-2))))))))));
}

/* The angle of (x, y) in [-pi, pi], as atan2, for finite or infinite doubles that are not NaN. */
static double angle(double y, double x, int x_negative, int y_negative) {
    double ax = x < 0 ? -x : x, ay = y < 0 ? -y : y, hi = ax > ay ? ax : ay, lo = ax > ay ? ay : ax, a;
    if (hi == 0.0) a = 0.0;
    else if (lo == hi) a = 0.78539816339744830962;                      /* equal, both infinite included */
    else if (hi > 3.5e38) a = 0.0;                                      /* one infinite, the other finite */
    else a = atan_unit(lo * recip_d(hi));
    if (ay > ax) a = 1.57079632679489661923 - a;
    if (x_negative) a = 3.14159265358979323846 - a;
    return y_negative ? -a : a;
}

static int sign_bit(float x) { return (int)(enaga_float_bits(x) >> 31); }

/* ---- Trigonometry ---- */

float F(sinf)(float x) {
    int q;
    double r;
    if (is_nan(x) || is_inf(x)) return nan_value();
    r = reduce(x, &q);
    return (float)(q == 0 ? sin_kernel(r) : q == 1 ? cos_kernel(r) : q == 2 ? -sin_kernel(r) : -cos_kernel(r));
}

float F(cosf)(float x) {
    int q;
    double r;
    if (is_nan(x) || is_inf(x)) return nan_value();
    r = reduce(x, &q);
    return (float)(q == 0 ? cos_kernel(r) : q == 1 ? -sin_kernel(r) : q == 2 ? -cos_kernel(r) : sin_kernel(r));
}

void F(sincosf)(float x, float *s, float *c) {
    int q;
    double r, sk, ck;
    if (is_nan(x) || is_inf(x)) {
        *s = *c = nan_value();
        return;
    }
    r = reduce(x, &q);
    sk = sin_kernel(r);
    ck = cos_kernel(r);
    *s = (float)(q == 0 ? sk : q == 1 ? ck : q == 2 ? -sk : -ck);
    *c = (float)(q == 0 ? ck : q == 1 ? -sk : q == 2 ? -ck : sk);
}

float F(tanf)(float x) {
    int q;
    double r, sk, ck;
    if (is_nan(x) || is_inf(x)) return nan_value();
    r = reduce(x, &q);
    sk = sin_kernel(r);
    ck = cos_kernel(r);
    return (float)(q & 1 ? -ck * recip_d(sk == 0.0 ? 1e-300 : sk) : sk * recip_d(ck));
}

float F(atan2f)(float y, float x) {
    if (is_nan(x) || is_nan(y)) return nan_value();
    return (float)angle(y, x, sign_bit(x), sign_bit(y));
}

float F(atanf)(float x) {
    if (is_nan(x)) return x;
    return (float)angle(x, 1.0, 0, sign_bit(x));
}

float F(asinf)(float x) {
    double xd = x;
    if (is_nan(x) || x > 1.0f || x < -1.0f) return nan_value();
    return (float)angle(xd, sqrt_d((1.0 - xd) * (1.0 + xd)), 0, sign_bit(x));
}

float F(acosf)(float x) {
    double xd = x;
    if (is_nan(x) || x > 1.0f || x < -1.0f) return nan_value();
    return (float)angle(sqrt_d((1.0 - xd) * (1.0 + xd)), xd, sign_bit(x), 0);
}

/* ---- Hyperbolic ---- */

float F(sinhf)(float x) {
    double a = x < 0 ? -(double)x : (double)x, e, r;
    if (is_nan(x) || is_inf(x)) return x;
    if (a < 1e-4) return x;                                             /* x + x^3/6: below float rounding */
    if (a > 20.0) {
        r = 0.5 * exp_d(a > 200.0 ? 200.0 : a);
    } else {
        e = expm1_d(a);
        r = 0.5 * (e + e * recip_d(e + 1.0));
    }
    return (float)(x < 0 ? -r : r);
}

float F(coshf)(float x) {
    double a = x < 0 ? -(double)x : (double)x, e;
    if (is_nan(x)) return x;
    if (is_inf(x)) return inf_value();
    e = exp_d(a > 200.0 ? 200.0 : a);
    return (float)(a > 20.0 ? 0.5 * e : 0.5 * (e + recip_d(e)));
}

float F(tanhf)(float x) {
    double a = x < 0 ? -(double)x : (double)x, e, r;
    if (is_nan(x)) return x;
    if (a < 1e-4) return x;
    if (a > 9.1) return x < 0 ? -1.0f : 1.0f;
    e = expm1_d(2.0 * a);
    r = e * recip_d(e + 2.0);
    return (float)(x < 0 ? -r : r);
}

/* ---- Exponentials and logarithms ---- */

float F(expf)(float x) {
    if (is_nan(x)) return x;
    return (float)exp_d(enaga_clamp(x, -200.0f, 200.0f));
}

float F(exp2f)(float x) {
    if (is_nan(x)) return x;
    return (float)exp2_d(enaga_clamp(x, -300.0f, 300.0f));
}

float F(expm1f)(float x) {
    if (is_nan(x)) return x;
    return (float)expm1_d(enaga_clamp(x, -200.0f, 200.0f));
}

/* The log of x, or what the standard gives for x <= 0, infinity and NaN (x itself, in the last two). */
static int log_special(float x, float *out) {
    if (is_nan(x) || x < 0.0f) *out = x < 0.0f ? nan_value() : x;
    else if (x == 0.0f) *out = -inf_value();
    else if (is_inf(x)) *out = x;
    else return 0;
    return 1;
}

float F(logf)(float x) {
    float special;
    return log_special(x, &special) ? special : (float)log_d(x);
}

float F(log2f)(float x) {
    float special;
    return log_special(x, &special) ? special : (float)(log_d(x) * 1.44269504088896340736);
}

float F(log10f)(float x) {
    float special;
    return log_special(x, &special) ? special : (float)(log_d(x) * 0.43429448190325182765);
}

float F(log1pf)(float x) {
    if (is_nan(x) || x < -1.0f) return nan_value();
    if (x == -1.0f) return -inf_value();
    if (is_inf(x)) return x;
    if (x > -6e-8f && x < 6e-8f) return x;                              /* x - x^2/2: below float rounding */
    return (float)log_d(1.0 + (double)x);                                /* 1 + x is exact in double */
}

/* An integer-valued float: 0 not an integer, 1 even, 2 odd. */
static int integer_kind(float y) {
    float a = y < 0 ? -y : y;
    int i;
    if (is_inf(y) || is_nan(y)) return 0;
    if (a >= 16777216.0f) return 1;                                     /* floats this large are even integers */
    i = (int)a;
    if ((float)i != a) return 0;
    return 1 + (i & 1);
}

float F(powf)(float x, float y) {
    int kind = integer_kind(y), negative = 0;
    double a, l;
    if (y == 0.0f || x == 1.0f) return 1.0f;
    if (is_nan(x) || is_nan(y)) return nan_value();
    if (is_inf(y)) {
        float ax = x < 0 ? -x : x;
        if (ax == 1.0f) return 1.0f;
        return (ax > 1.0f) == (y > 0.0f) ? inf_value() : 0.0f;
    }
    if (sign_bit(x)) {
        if (x != 0.0f && !is_inf(x) && kind == 0) return nan_value();
        negative = kind == 2;
    }
    if (x == 0.0f) {
        float r = y < 0.0f ? inf_value() : 0.0f;
        return negative ? -r : r;
    }
    if (is_inf(x)) {
        float r = y < 0.0f ? 0.0f : inf_value();
        return negative ? -r : r;
    }
    a = x < 0 ? -(double)x : (double)x;
    l = (double)y * log_d(a) * 1.44269504088896340736;
    l = l > 300.0 ? 300.0 : l < -300.0 ? -300.0 : l;
    a = exp2_d(l);
    return (float)(negative ? -a : a);
}

float F(sqrtf)(float x) {
    if (is_nan(x) || x < 0.0f) return x < 0.0f ? nan_value() : x;
    if (x == 0.0f || is_inf(x)) return x;
    return (float)sqrt_d(x);
}

float F(cbrtf)(float x) {
    double a, r;
    if (is_nan(x) || is_inf(x) || x == 0.0f) return x;
    a = x < 0 ? -(double)x : (double)x;
    r = exp2_d(log_d(a) * 1.44269504088896340736 * 0.33333333333333333333);
    r = r - (r * r * r - a) * recip_d(3.0 * r * r);                    /* one Newton step */
    return (float)(x < 0 ? -r : r);
}

float F(hypotf)(float x, float y) {
    double xd = x, yd = y;
    if (is_inf(x) || is_inf(y)) return inf_value();
    if (is_nan(x) || is_nan(y)) return nan_value();
    return (float)sqrt_d(xd * xd + yd * yd);
}

/* ---- Rounding, remainders, parts ---- */

float F(fabsf)(float x) { return enaga_abs(x); }
float F(copysignf)(float x, float s) { return enaga_copysign(x, s); }

float F(truncf)(float x) {
    uint32_t bits = enaga_float_bits(x);
    int e = (int)((bits >> 23) & 0xFF) - 127;
    if (e >= 23) return x;                                              /* an integer already, or inf or NaN */
    if (e < 0) return enaga_bits_float(bits & 0x80000000u);
    return enaga_bits_float(bits & ~(0x007FFFFFu >> e));
}

float F(floorf)(float x) {
    float t = F(truncf)(x);
    return t > x ? t - 1.0f : t;
}

float F(ceilf)(float x) {
    float t = F(truncf)(x);
    return t < x ? t + 1.0f : t;
}

float F(roundf)(float x) {                                              /* halves away from zero */
    float t = F(truncf)(x), d = x - t;
    if (d >= 0.5f) return t + 1.0f;
    if (d <= -0.5f) return t - 1.0f;
    return t;
}

float F(rintf)(float x) {                                               /* halves to even */
    float a = enaga_abs(x), r;
    volatile float big = 8388608.0f;                                    /* kept from being folded away */
    if (!(a < 8388608.0f)) return x;
    r = (a + big) - big;
    return enaga_copysign(r, x);
}

float F(nearbyintf)(float x) { return F(rintf)(x); }
long F(lroundf)(float x) { return (long)F(roundf)(x); }
long F(lrintf)(float x) { return (long)F(rintf)(x); }

float F(fminf)(float x, float y) { return is_nan(x) ? y : is_nan(y) ? x : x < y ? x : y; }
float F(fmaxf)(float x, float y) { return is_nan(x) ? y : is_nan(y) ? x : x > y ? x : y; }
float F(fdimf)(float x, float y) { return is_nan(x) || is_nan(y) ? nan_value() : x > y ? x - y : 0.0f; }

/* The exact remainder x - n y, n = trunc(x / y): long division of the mantissas. */
float F(fmodf)(float x, float y) {
    uint32_t bx = enaga_float_bits(x), by = enaga_float_bits(y), sign = bx & 0x80000000u, mx, my;
    int ex = (int)((bx >> 23) & 0xFF), ey = (int)((by >> 23) & 0xFF);
    bx &= 0x7FFFFFFFu;
    by &= 0x7FFFFFFFu;
    if (by == 0 || ex == 0xFF || is_nan(y)) return nan_value();
    if (bx < by) return x;
    if (bx == by) return enaga_bits_float(sign);
    if (ex == 0) {                                                      /* subnormal: normalise */
        for (mx = bx << 8; !(mx & 0x80000000u); mx <<= 1) ex--;
        mx >>= 8;
        ex++;
    } else {
        mx = (bx & 0x007FFFFFu) | 0x00800000u;
    }
    if (ey == 0) {
        for (my = by << 8; !(my & 0x80000000u); my <<= 1) ey--;
        my >>= 8;
        ey++;
    } else {
        my = (by & 0x007FFFFFu) | 0x00800000u;
    }
    for (; ex > ey; ex--) {
        if (mx >= my) mx -= my;
        mx <<= 1;
    }
    if (mx >= my) mx -= my;
    if (mx == 0) return enaga_bits_float(sign);
    for (; mx < 0x00800000u; mx <<= 1) ey--;
    if (ey > 0) return enaga_bits_float(sign | (uint32_t)ey << 23 | (mx & 0x007FFFFFu));
    return enaga_bits_float(sign | mx >> (1 - ey));                     /* subnormal result, exact */
}

float F(ldexpf)(float x, int n) {
    if (is_nan(x) || is_inf(x) || x == 0.0f) return x;
    n = n > 300 ? 300 : n < -300 ? -300 : n;
    return (float)((double)x * power2(n));
}

float F(scalbnf)(float x, int n) { return F(ldexpf)(x, n); }

float F(frexpf)(float x, int *e) {
    double d = x;
    uint32_t high;
    if (is_nan(x) || is_inf(x) || x == 0.0f) {
        *e = 0;
        return x;
    }
    high = high_word(d);                                                /* as a double, a float is never subnormal */
    *e = (int)((high >> 20) & 0x7FF) - 1022;
    return (float)with_high(d, (high & 0x800FFFFFu) | 0x3FE00000u);     /* [0.5, 1) */
}

float F(modff)(float x, float *whole) {
    *whole = F(truncf)(x);
    if (is_inf(x)) return enaga_copysign(0.0f, x);
    return enaga_copysign(x - *whole, x);
}
