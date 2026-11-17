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

/* ---- Wavetables ---- */

void enaga_wavetable_add(enaga_oscillator *o, const float *restrict table, int bits, float *restrict out, int n,
                         float dt, float level) {
    uint32_t p = o->phase;
    const uint32_t dp = (uint32_t)(int32_t)(dt * TURN), shift = 32u - (uint32_t)bits;
    int i;
    for (i = 0; i < n; i++) {
        const uint32_t j = p >> shift;
        const float f = enaga_unipolar(p << bits), a = table[j];
        out[i] += level * (a + (table[j + 1] - a) * f);
        p += dp;
    }
    o->phase = p;
}

void enaga_wavetable_morph_add(enaga_oscillator *o, const float *restrict a, const float *restrict b, int bits,
                               float morph, float *restrict out, int n, float dt, float level) {
    uint32_t p = o->phase;
    const uint32_t dp = (uint32_t)(int32_t)(dt * TURN), shift = 32u - (uint32_t)bits;
    int i;
    for (i = 0; i < n; i++) {
        const uint32_t j = p >> shift;
        const float f = enaga_unipolar(p << bits), a0 = a[j], b0 = b[j];
        const float ya = a0 + (a[j + 1] - a0) * f, yb = b0 + (b[j + 1] - b0) * f;
        out[i] += level * (ya + (yb - ya) * morph);
        p += dp;
    }
    o->phase = p;
}

/* ---- Hard sync ----
 *
 * Each sample is the naive wave plus, for every jump of height J that happened a fraction a of a sample ago, the
 * two-sample PolyBLEP correction: -J/2 (1 - a)^2 on this sample and J/2 a^2 on the one before (the same residual
 * as blep() above). So a sample is finished only when the next one's jumps are known: the loop computes the phase
 * a sample ahead and writes the sample before, which keeps it in step with enaga_oscillator_add. */

/* The naive waves, as expressions of the phase x (and the pulse's width w). */
#define NAIVE_SAW(x, w) (2.0f * (x) - 1.0f)
#define NAIVE_PULSE(x, w) ((x) < (w) ? 1.0f : -1.0f)
#define NAIVE_TRIANGLE(x, w) ((x) < 0.5f ? 4.0f * (x) - 1.0f : 3.0f - 4.0f * (x))
#define NAIVE_SINE(x, w) enaga_sin_turns(x)

/* One wave's loop, written out by a macro for each wave so the loop is specialised. WRAP_JUMP is the wave's own
 * jump at the end of a cycle: saw +1 to -1, pulse -1 to +1; triangle and sine are continuous. For cl6x the loop is
 * branch-free (both cases computed, one selected), which lets it pipeline; GCC, which doesn't pipeline, is faster
 * branching around the restart's work (about one sample in a hundred). The arithmetic, so the result, is the
 * same. */
#ifdef __TI_COMPILER_VERSION__
#define SYNC_RESTART(NAIVE)                                                                                        \
    const float a = enaga_clamp(enaga_unipolar(pm1) * inv_dt_m, 0.0f, 1.0f);                                      \
    const float at = enaga_unipolar(p + (uint32_t)(int32_t)((1.0f - a) * dt * TURN));                             \
    const uint32_t p1 = restart ? (uint32_t)(int32_t)(a * dt * TURN) : free;                                       \
    const float x0 = enaga_unipolar(p), x1 = enaga_unipolar(p1);                                                  \
    const float jump = restart ? NAIVE(0.0f, width) - NAIVE(at, width) : wrap ? wrap_jump : 0.0f;                 \
    const float since = restart ? a : x1 * inv_dt;
#else
#define SYNC_RESTART(NAIVE)                                                                                        \
    float a = 0.0f, at = 0.0f, jump, since;                                                                        \
    uint32_t p1 = free;                                                                                            \
    if (restart) {                                                                                                 \
        a = enaga_clamp(enaga_unipolar(pm1) * inv_dt_m, 0.0f, 1.0f);                                              \
        at = enaga_unipolar(p + (uint32_t)(int32_t)((1.0f - a) * dt * TURN));                                     \
        p1 = (uint32_t)(int32_t)(a * dt * TURN);                                                                   \
    }                                                                                                              \
    const float x0 = enaga_unipolar(p), x1 = enaga_unipolar(p1);                                                  \
    if (restart) {                                                                                                 \
        jump = NAIVE(0.0f, width) - NAIVE(at, width);                                                              \
        since = a;                                                                                                 \
    } else {                                                                                                       \
        jump = wrap ? wrap_jump : 0.0f;                                                                            \
        since = x1 * inv_dt;                                                                                       \
    }
#endif
#define SYNC_LOOP(NAIVE, PULSE, WRAP_JUMP)                                                                         \
    for (i = 0; i < n; i++) {                                                                                      \
        const float wrap_jump = (WRAP_JUMP);                                                                       \
        const uint32_t pm1 = pm + dpm, free = p + dp;                                                              \
        const int restart = pm1 < pm, wrap = free < p;                                                             \
        /* If the master wrapped, a samples ago, the slave stopped at `at` and restarted; the jump this sample is \
         * the restart's, or else the wave's own wrap, `since` samples ago. */                                     \
        SYNC_RESTART(NAIVE)                                                                                        \
        float cur = NAIVE(x1, width) - 0.5f * jump * (1.0f - since) * (1.0f - since);                             \
        prev += 0.5f * jump * since * since;                                                                       \
        if (PULSE) {    /* the falling edge at width: on the way to a restart, or in a free sample */             \
            const int edge = (x0 < width) & (restart ? at >= width : (!wrap) & (x1 >= width));                    \
            const float e = restart ? a + (at - width) * inv_dt : (x1 - width) * inv_dt;                          \
            cur += edge ? (1.0f - e) * (1.0f - e) : 0.0f;                                                          \
            prev -= edge ? e * e : 0.0f;                                                                           \
        }                                                                                                          \
        out[i] += level * (prev + dc);                                                                             \
        prev = cur;                                                                                                \
        pm = pm1;                                                                                                  \
        p = p1;                                                                                                    \
    }

void enaga_oscillator_sync_add(enaga_oscillator *o, float *carry, const enaga_oscillator *master, float dt_m,
                               float inv_dt_m, float *restrict out, int n, int wave, float dt, float inv_dt,
                               float level, float width) {
    const uint32_t dpm = (uint32_t)(int32_t)(dt_m * TURN), dp = (uint32_t)(int32_t)(dt * TURN);
    uint32_t pm = master->phase, p = o->phase;
    float prev = *carry, dc = 0.0f;
    int i;
    width = enaga_clamp(width, 0.05f, 0.95f);
    switch (wave) {
    case ENAGA_WAVE_SAW: SYNC_LOOP(NAIVE_SAW, 0, -2.0f) break;
    case ENAGA_WAVE_PULSE:
        dc = 1.0f - 2.0f * width;       /* DC removed, as enaga_oscillator_add's pulse */
        SYNC_LOOP(NAIVE_PULSE, 1, 2.0f)
        break;
    case ENAGA_WAVE_TRIANGLE: SYNC_LOOP(NAIVE_TRIANGLE, 0, 0.0f) break;
    default: SYNC_LOOP(NAIVE_SINE, 0, 0.0f) break;
    }
    o->phase = p;
    *carry = prev;
}
