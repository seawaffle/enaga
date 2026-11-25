/* Enaga: <math.h> for apps, the float functions of the C standard (runtime/libm.c), within 1 ulp. Apps get no C
 * library, so tools/build_app.py puts this directory on GCC's system include path: code written against libm
 * builds unchanged, as long as it uses the float functions (sinf, not sin). The double functions are left out, so
 * calling one is a compile error rather than a quietly slow double computation. For inner loops,
 * <enaga/dsp/math.h> is several times faster (docs/api.md, "Math"). */
#ifndef ENAGA_LIBM_MATH_H
#define ENAGA_LIBM_MATH_H

#ifdef __cplusplus
extern "C" {
#endif

typedef float float_t;
typedef double double_t;

#define INFINITY (__builtin_inff())
#define NAN (__builtin_nanf(""))
#define HUGE_VALF (__builtin_huge_valf())
#define HUGE_VAL (__builtin_huge_val())

#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define isnormal(x) __builtin_isnormal(x)
#define signbit(x) __builtin_signbit(x)

/* POSIX's constants. They are doubles, as everywhere: in float code write (float)M_PI, as a double constant makes
 * the whole expression double. */
#define M_E 2.7182818284590452354
#define M_LOG2E 1.4426950408889634074
#define M_LOG10E 0.43429448190325182765
#define M_LN2 0.69314718055994530942
#define M_LN10 2.30258509299404568402
#define M_PI 3.14159265358979323846
#define M_PI_2 1.57079632679489661923
#define M_PI_4 0.78539816339744830962
#define M_1_PI 0.31830988618379067154
#define M_2_PI 0.63661977236758134308
#define M_2_SQRTPI 1.12837916709551257390
#define M_SQRT2 1.41421356237309504880
#define M_SQRT1_2 0.70710678118654752440

float sinf(float x);
float cosf(float x);
void sincosf(float x, float *s, float *c);
float tanf(float x);
float asinf(float x);
float acosf(float x);
float atanf(float x);
float atan2f(float y, float x);
float sinhf(float x);
float coshf(float x);
float tanhf(float x);
float expf(float x);
float exp2f(float x);
float expm1f(float x);
float logf(float x);
float log2f(float x);
float log10f(float x);
float log1pf(float x);
float powf(float x, float y);
float sqrtf(float x);
float cbrtf(float x);
float hypotf(float x, float y);
float fabsf(float x);
float copysignf(float x, float s);
float truncf(float x);
float floorf(float x);
float ceilf(float x);
float roundf(float x);
float rintf(float x);
float nearbyintf(float x);
long lroundf(float x);
long lrintf(float x);
float fminf(float x, float y);
float fmaxf(float x, float y);
float fdimf(float x, float y);
float fmodf(float x, float y);
float ldexpf(float x, int n);
float scalbnf(float x, int n);
float frexpf(float x, int *e);
float modff(float x, float *whole);

#ifdef __cplusplus
}
#endif

#endif
