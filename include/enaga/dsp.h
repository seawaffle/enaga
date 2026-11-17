/* Enaga DSP library, C interface. Provisional until 1.0 (docs/design.md, "What a developer gets").
 *
 * C++ apps normally use <enaga/dsp.hpp>, whose classes wrap the state structs and functions here. This header is
 * for C, including an app's own *.ti.c files: the state is plain structs, so custom loops can work on the same
 * state as the library's.
 *
 * Two kinds of function:
 *  - per-sample steps (static inline here): for code that must run sample by sample, such as feedback loops;
 *  - block functions (the .c files in runtime/dsp): a whole buffer per call. The app build compiles them with TI's cl6x when
 *    it is installed (several times faster than GCC's code), with GCC otherwise, and links them into the app.
 *    Prefer these in the audio callback: process in blocks.
 *
 * Buffers are float. Unless a function says otherwise, distinct buffer arguments must not overlap; a function
 * that works in place takes one buffer. */
#ifndef ENAGA_DSP_H
#define ENAGA_DSP_H

#include <stdint.h>

#include <enaga/dsp/math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Buffers ---- */

void enaga_clear(float *buf, int n);                                     /* buf = 0 */
void enaga_add(float *out, const float *in, int n);                      /* out += in */
void enaga_mix(float *out, const float *in, int n, float gain);          /* out += gain * in */
void enaga_scale(float *buf, int n, float gain);                         /* buf *= gain */
void enaga_multiply(float *buf, const float *gain, int n);               /* buf *= gain (sample by sample) */
void enaga_soft_clip_buffer(float *buf, int n, float drive);             /* buf = soft_clip(drive * buf) */
/* left += in * gl, right += in * gr, where gl and gr step by step_l and step_r before each sample (a gain ramp
 * across the block); *gl and *gr are left at their final values. */
void enaga_ramp_stereo_add(const float *in, float *left, float *right, int n, float *gl, float *gr,
                           float step_l, float step_r);

/* ---- Noise ---- */

/* Adds level x white noise to out (enaga_xorshift, as enaga_bipolar). *state must be non-zero. */
void enaga_noise_add(uint32_t *state, float *out, int n, float level);

/* ---- Oscillators: PolyBLEP saw and pulse, triangle, sine ---- */

enum { ENAGA_WAVE_SAW, ENAGA_WAVE_PULSE, ENAGA_WAVE_TRIANGLE, ENAGA_WAVE_SINE };

/* The phase is a 32-bit fraction of a turn: it wraps by itself, which keeps the per-sample chain short. */
typedef struct { uint32_t phase; } enaga_oscillator;

static inline float enaga_oscillator_phase(const enaga_oscillator *o) { return enaga_unipolar(o->phase); }
static inline void enaga_oscillator_reset(enaga_oscillator *o, float phase) {
    o->phase = (uint32_t)(int32_t)(phase * 16777216.0f) << 8;
}
/* Adds level x the wave to out. dt (cycles per sample) in (0, 0.5) with inv_dt = 1 / dt, computed by the caller
 * at control rate; pulse width 0.05-0.95. */
void enaga_oscillator_add(enaga_oscillator *o, float *out, int n, int wave, float dt, float inv_dt, float level,
                          float width);

/* ---- State-variable filter (topology-preserving, trapezoidal) ---- */

/* a1 = 1 / (1 + g (g + k)), a2 = g a1, a3 = g a2, with g = tan(pi cutoff / rate) and k = 1 / Q (2 for none). */
typedef struct { float a1, a2, a3, k; } enaga_svf_coefficients;
typedef struct { float s1, s2; } enaga_svf;

enum { ENAGA_SVF_LOW, ENAGA_SVF_BAND, ENAGA_SVF_HIGH };

/* One sample: updates the state; *low, *band and *high get the three outputs. */
static inline void enaga_svf_step(enaga_svf *s, const enaga_svf_coefficients *c, float x, float *low, float *band,
                                  float *high) {
    float v3 = x - s->s2;
    float v1 = c->a1 * s->s1 + c->a2 * v3;
    float v2 = s->s2 + c->a2 * s->s1 + c->a3 * v3;
    s->s1 = 2.0f * v1 - s->s1;
    s->s2 = 2.0f * v2 - s->s2;
    *low = v2;
    *band = v1;
    *high = x - c->k * v1 - v2;
}
/* out = one output (ENAGA_SVF_LOW, _BAND or _HIGH) of in. out may be in. */
void enaga_svf_process(enaga_svf *s, const enaga_svf_coefficients *c, int output, const float *in, float *out,
                       int n);
/* Two filters in series, both giving `output` (a 24 dB low-pass from two low-passes, say): the same as two
 * enaga_svf_process calls, about twice as fast, since the two stages overlap. out may be in. */
void enaga_svf_cascade(enaga_svf *s1, const enaga_svf_coefficients *c1, enaga_svf *s2,
                       const enaga_svf_coefficients *c2, int output, const float *in, float *out, int n);

/* ---- One-pole low-pass: y += a (x - y) ---- */

typedef struct { float y; } enaga_one_pole;
static inline float enaga_one_pole_step(enaga_one_pole *p, float x, float a) { return p->y += a * (x - p->y); }
void enaga_one_pole_process(enaga_one_pole *p, float a, const float *in, float *out, int n);   /* out may be in */

/* ---- ADSR envelope: one-pole segments ---- */

enum { ENAGA_ENV_IDLE, ENAGA_ENV_ATTACK, ENAGA_ENV_DECAY, ENAGA_ENV_SUSTAIN, ENAGA_ENV_RELEASE };

/* One-pole coefficients per step, and the sustain level (0-1). See EnvelopeShape::make in modulation.hpp. */
typedef struct { float attack, decay, sustain, release; } enaga_envelope_shape;
typedef struct { int32_t stage; float level; } enaga_envelope;

#define ENAGA_ENV_ATTACK_TARGET 1.3f    /* the attack heads here and stops at 1: a curve like an analogue one */

/* One step; returns the new level. */
static inline float enaga_envelope_step(enaga_envelope *e, const enaga_envelope_shape *s) {
    float l = e->level;
    switch (e->stage) {
    case ENAGA_ENV_ATTACK:
        l += s->attack * (ENAGA_ENV_ATTACK_TARGET - l);
        if (l >= 1.0f) { l = 1.0f; e->stage = ENAGA_ENV_DECAY; }
        break;
    case ENAGA_ENV_DECAY:
        l += s->decay * (s->sustain - l);
        if (l - s->sustain < 1e-4f) e->stage = ENAGA_ENV_SUSTAIN;
        break;
    case ENAGA_ENV_SUSTAIN:
        l += s->decay * (s->sustain - l);          /* follows sustain changes smoothly */
        break;
    case ENAGA_ENV_RELEASE:
        l += s->release * (-0.001f - l);           /* aims just below zero so it ends in finite time */
        if (l <= 0.0f) { l = 0.0f; e->stage = ENAGA_ENV_IDLE; }
        break;
    default:
        break;
    }
    return e->level = l;
}
/* n steps into out: the same values as n calls of enaga_envelope_step. */
void enaga_envelope_render(enaga_envelope *e, const enaga_envelope_shape *s, float *out, int n);

#ifdef __cplusplus
}
#endif

#endif
