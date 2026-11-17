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
/* a = 1 - exp(-2 pi cutoff / rate) */
static inline float enaga_one_pole_coefficient(float cutoff, float rate) {
    return 1.0f - enaga_exp(-2.0f * ENAGA_PI * cutoff / rate);
}
static inline float enaga_one_pole_step(enaga_one_pole *p, float x, float a) { return p->y += a * (x - p->y); }
void enaga_one_pole_process(enaga_one_pole *p, float a, const float *in, float *out, int n);   /* out may be in */

/* ---- Delay lines ---- */

/* A ring of a power-of-two size: buffer[write] is the last sample written. The memory is the caller's
 * (enaga_delay_size floats, zeroed); DelayLine in effects.hpp allocates it on the app heap. */
typedef struct { float *buffer; uint32_t mask, write; } enaga_delay_line;

/* The buffer size for delays up to max_samples. */
static inline uint32_t enaga_delay_size(uint32_t max_samples) {
    uint32_t size = 1;
    while (size < max_samples + 2) size <<= 1;
    return size;
}
static inline void enaga_delay_init(enaga_delay_line *d, float *buffer, uint32_t size) {
    d->buffer = buffer;
    d->mask = size - 1;
    d->write = 0;
}
static inline uint32_t enaga_delay_capacity(const enaga_delay_line *d) { return d->mask - 1; }
static inline void enaga_delay_write(enaga_delay_line *d, float x) { d->buffer[d->write = (d->write + 1) & d->mask] = x; }
/* The sample written `delay` writes ago (0 = the last). */
static inline float enaga_delay_tap(const enaga_delay_line *d, uint32_t delay) {
    return d->buffer[(d->write - delay) & d->mask];
}
/* A fractional delay, 0 to capacity, linearly interpolated. */
static inline float enaga_delay_tap_linear(const enaga_delay_line *d, float delay) {
    int whole = (int)delay;
    float frac = delay - (float)whole;
    return enaga_lerp(enaga_delay_tap(d, (uint32_t)whole), enaga_delay_tap(d, (uint32_t)whole + 1), frac);
}
/* Block reads and writes. In a block of n samples whose writes come after its reads, sample i would see
 * tap(delay) = out[i] of enaga_delay_read(d, delay, out, n), provided delay >= n - 1 (so every sample read was
 * written before the block). Reading the block first and writing it after keeps the loops free of memory
 * dependencies, which is what lets them pipeline. */
void enaga_delay_read(const enaga_delay_line *d, uint32_t delay, float *out, int n);
void enaga_delay_write_block(enaga_delay_line *d, const float *in, int n);

/* ---- Stereo delay: feedback through a low-pass (tone), optional ping-pong, a gliding time ---- */

typedef struct {
    enaga_delay_line left, right;
    enaga_one_pole damp_l, damp_r;
    float rate, current, target, feedback, tone;    /* current and target: the delay in samples */
    int32_t ping_pong;
} enaga_stereo_delay;

/* The floats of memory enaga_stereo_delay_init needs (zeroed) for delays up to max_seconds. */
uint32_t enaga_stereo_delay_memory(float max_seconds, float rate);
void enaga_stereo_delay_init(enaga_stereo_delay *s, float *memory, float max_seconds, float rate);
/* seconds; feedback 0-0.95; tone 0 (dark) to 1 (bright); ping_pong crosses the feedback. The time glides there. */
void enaga_stereo_delay_set(enaga_stereo_delay *s, float seconds, float feedback, float tone, int ping_pong);
/* Feeds in_l / in_r through the delay and adds mix x the echoes to out_l / out_r. */
void enaga_stereo_delay_process(enaga_stereo_delay *s, const float *in_l, const float *in_r, float *out_l,
                                float *out_r, int n, float mix);

/* ---- Reverb: four Schroeder all-passes into an eight-line Hadamard feedback delay network ---- */

#define ENAGA_REVERB_LINES 8
#define ENAGA_REVERB_DIFFUSERS 4

typedef struct {
    enaga_delay_line lines[ENAGA_REVERB_LINES], diffusers[ENAGA_REVERB_DIFFUSERS];
    enaga_one_pole damp[ENAGA_REVERB_LINES];
    uint32_t length[ENAGA_REVERB_LINES], diffuse_length[ENAGA_REVERB_DIFFUSERS];
    float gain[ENAGA_REVERB_LINES], damping, rate;
} enaga_reverb;

/* The floats of memory enaga_reverb_init needs (zeroed) at this sample rate. */
uint32_t enaga_reverb_memory(float rate);
void enaga_reverb_init(enaga_reverb *r, float *memory, float rate);
/* decay_s: time to fall 60 dB (0.2-30); damping 0 (bright) to 1 (dark). */
void enaga_reverb_set(enaga_reverb *r, float decay_s, float damping);
/* Adds mix x the reverb of in_l + in_r to out_l / out_r. */
void enaga_reverb_process(enaga_reverb *r, const float *in_l, const float *in_r, float *out_l, float *out_r, int n,
                          float mix);

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
