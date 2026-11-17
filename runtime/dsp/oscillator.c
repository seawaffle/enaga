/* Enaga DSP: oscillators (include/enaga/dsp.h). Built by cl6x when installed, else GCC.
 *
 * For cl6x the loops have no branches (it won't software-pipeline a loop that has any): the phase wraps by itself
 * as a 32-bit fraction of a turn, and the PolyBLEP computes both of its corrections and selects one. GCC doesn't
 * pipeline either way and is faster branching around the corrections; the arithmetic, so the result, is the same. */
#include <enaga/dsp.h>

#define TURN 4294967296.0f

/* The residual that takes the step out of a naive saw or pulse at a discontinuity at phase 0. */
static inline float blep(float t, float dt, float inv_dt) {
#ifdef __TI_COMPILER_VERSION__
    float a = t * inv_dt, b = (t - 1.0f) * inv_dt;
    float r = t < dt ? a + a - a * a - 1.0f : 0.0f;
    return t > 1.0f - dt ? b * b + b + b + 1.0f : r;
#else
    if (t < dt) { float a = t * inv_dt; return a + a - a * a - 1.0f; }
    if (t > 1.0f - dt) { float b = (t - 1.0f) * inv_dt; return b * b + b + b + 1.0f; }
    return 0.0f;
#endif
}

void enaga_oscillator_add(enaga_oscillator *o, float *restrict out, int n, int wave, float dt, float inv_dt,
                          float level, float width) {
    uint32_t p = o->phase;
    const uint32_t dp = (uint32_t)(int32_t)(dt * TURN);
    int i;
    switch (wave) {
    case ENAGA_WAVE_SAW:
        for (i = 0; i < n; i++) {
            float x = enaga_unipolar(p);
            out[i] += level * (2.0f * x - 1.0f - blep(x, dt, inv_dt));
            p += dp;
        }
        break;
    case ENAGA_WAVE_PULSE: {
        width = enaga_clamp(width, 0.05f, 0.95f);
        const uint32_t shift = (uint32_t)(int32_t)((1.0f - width) * 16777216.0f) << 8;
        const float dc = 1.0f - 2.0f * width;     /* DC removed, so width changes don't thump */
        for (i = 0; i < n; i++) {
            float x = enaga_unipolar(p), x2 = enaga_unipolar(p + shift);
            float y = (x < width ? 1.0f : -1.0f) + blep(x, dt, inv_dt) - blep(x2, dt, inv_dt);
            out[i] += level * (y + dc);
            p += dp;
        }
        break;
    }
    case ENAGA_WAVE_TRIANGLE:
        for (i = 0; i < n; i++) {
            float x = enaga_unipolar(p), y = 4.0f * x - 1.0f;
            out[i] += level * (x < 0.5f ? y : 2.0f - y);
            p += dp;
        }
        break;
    default:
        for (i = 0; i < n; i++) {
            out[i] += level * enaga_sin_turns(enaga_unipolar(p));
            p += dp;
        }
        break;
    }
    o->phase = p;
}
