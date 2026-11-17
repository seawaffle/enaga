/* Built by cl6x when it is installed, otherwise by GCC (tools/build_app.py). Keep it plain C. */
#include "voice.h"

static inline float clamp(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

/* enaga::dsp::soft_clip */
static inline float soft_clip(float x) {
    x = clamp(x, -1.5f, 1.5f);
    return x - x * x * x * (4.0f / 27.0f);
}

/* enaga::dsp::floor_int and sin_turns */
static inline int floor_int(float x) {
    int i = (int)x;
    return (float)i > x ? i - 1 : i;
}
static inline float sin_turns(float t) {
    t -= (float)floor_int(t + 0.5f);
    if (t > 0.25f) t = 0.5f - t;
    else if (t < -0.25f) t = -0.5f - t;
    float t2 = t * t;
    return t * (6.28318531f + t2 * (-41.3417022f + t2 * (81.6052493f + t2 * (-76.7058597f + t2 * 42.0586939f))));
}

/* enaga::dsp::Oscillator::blep, without branches: both corrections are computed and one is selected, so
 * cl6x can software-pipeline the loops that use it (it won't pipeline a loop containing branches). */
static inline float blep(float t, float dt, float inv_dt) {
    float a = t * inv_dt, b = (t - 1.0f) * inv_dt;
    float r = t < dt ? a + a - a * a - 1.0f : 0.0f;
    return t > 1.0f - dt ? b * b + b + b + 1.0f : r;
}

/* The phase runs as a 32-bit fraction of a turn, which wraps by itself: no compare-and-subtract chain from one
 * sample to the next. Its top 24 bits, as a float in [0, 1), are the t of enaga::dsp::Oscillator. */
#define TURN 4294967296.0f
static inline float turns(uint32_t p) { return (float)(p >> 8) * (1.0f / 16777216.0f); }

float synth_oscillator(float *restrict out, int n, int wave, float t, float dt, float inv_dt, float level,
                       float width) {
    uint32_t p = (uint32_t)(int32_t)(t * 16777216.0f) << 8;
    const uint32_t dp = (uint32_t)(int32_t)(dt * TURN);
    int i;
    switch (wave) {
    case SYNTH_SAW:
        for (i = 0; i < n; i++) {
            float x = turns(p);
            out[i] += level * (2.0f * x - 1.0f - blep(x, dt, inv_dt));
            p += dp;
        }
        break;
    case SYNTH_PULSE: {
        width = clamp(width, 0.05f, 0.95f);
        const uint32_t shift = (uint32_t)(int32_t)((1.0f - width) * 16777216.0f) << 8;
        const float dc = 1.0f - 2.0f * width;     /* DC removed, so width changes don't thump */
        for (i = 0; i < n; i++) {
            float x = turns(p), x2 = turns(p + shift);
            float y = (x < width ? 1.0f : -1.0f) + blep(x, dt, inv_dt) - blep(x2, dt, inv_dt);
            out[i] += level * (y + dc);
            p += dp;
        }
        break;
    }
    case SYNTH_TRIANGLE:
        for (i = 0; i < n; i++) {
            float x = turns(p), y = 4.0f * x - 1.0f;
            out[i] += level * (x < 0.5f ? y : 2.0f - y);
            p += dp;
        }
        break;
    default:
        for (i = 0; i < n; i++) {
            out[i] += level * sin_turns(turns(p));
            p += dp;
        }
        break;
    }
    return turns(p);
}

void synth_noise(float *restrict out, int n, float level, uint32_t *state) {
    uint32_t x = *state;
    int i;
    for (i = 0; i < n; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        out[i] += level * ((float)(int32_t)x * (1.0f / 2147483648.0f));
    }
    *state = x;
}

/* One step of enaga::dsp::Svf: updates the state and returns v1 (band); *low gets v2. */
static inline float svf_step(SynthSvfState *s, float x, const SynthSvfCoefficients *c, float *low) {
    float v3 = x - s->s2;
    float v1 = c->a1 * s->s1 + c->a2 * v3;
    float v2 = s->s2 + c->a2 * s->s1 + c->a3 * v3;
    s->s1 = 2.0f * v1 - s->s1;
    s->s2 = 2.0f * v2 - s->s2;
    *low = v2;
    return v1;
}

void synth_filter(float *restrict buf, int n, int type, float drive, const SynthSvfCoefficients *c1,
                  const SynthSvfCoefficients *c2, SynthSvfState *f1, SynthSvfState *f2) {
    /* Coefficients and state in locals, so the loops keep them in registers. */
    SynthSvfCoefficients a = *c1, b = *c2;
    SynthSvfState s = *f1, t = *f2;
    float low, band;
    int i;
    switch (type) {
    case SYNTH_LP12:
        for (i = 0; i < n; i++) { svf_step(&t, soft_clip(drive * buf[i]), &b, &low); buf[i] = low; }
        break;
    case SYNTH_LP24:
        for (i = 0; i < n; i++) {
            svf_step(&s, soft_clip(drive * buf[i]), &a, &low);
            svf_step(&t, low, &b, &low);
            buf[i] = low;
        }
        break;
    case SYNTH_BP:
        for (i = 0; i < n; i++) buf[i] = svf_step(&t, soft_clip(drive * buf[i]), &b, &low);
        break;
    default:
        for (i = 0; i < n; i++) {
            float x = soft_clip(drive * buf[i]);
            band = svf_step(&t, x, &b, &low);
            buf[i] = x - b.k * band - low;
        }
        break;
    }
    *f1 = s;
    *f2 = t;
}
