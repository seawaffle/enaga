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
void enaga_mix_product(float *out, const float *a, const float *b, int n, float gain);   /* out += gain * a * b */
/* Math on a buffer: the same values as math.h's functions, several times faster under cl6x than calling those
 * one value at a time. For control-rate values too: gather a step's pitches or cutoffs of all voices into one
 * array and convert them in one call. cl6x's pipelined loops for exp2 and tan_pi need 10 values or more; fewer
 * take a slower plain loop, so pad a short buffer (8 voices' values plus 8 spare cost less than 8). */
void enaga_exp2_buffer(float *buf, int n);                               /* buf = enaga_exp2(buf) */
void enaga_reciprocal_buffer(float *buf, int n);                         /* buf = enaga_reciprocal(buf) */
void enaga_tan_pi_buffer(float *buf, int n);                             /* buf = enaga_tan_pi(buf) */
void enaga_sqrt_buffer(float *buf, int n);                               /* buf = enaga_sqrt(buf) */
void enaga_tanh_buffer(float *buf, int n, float drive);                  /* buf = enaga_tanh(drive * buf) */
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
/* Moves the phase on n samples without sound, exactly as enaga_oscillator_add would: for a silent sync master. */
static inline void enaga_oscillator_advance(enaga_oscillator *o, int n, float dt) {
    o->phase += (uint32_t)n * (uint32_t)(int32_t)(dt * 4294967296.0f);
}
/* Hard sync: adds level x the wave of o, its phase restarted whenever master's wraps, to out. master (dt_m, with
 * inv_dt_m = 1 / dt_m) is read, not moved on: call this before rendering master's own block. Every jump, the
 * restarts' and the wave's own, is band-limited by a correction spread over two samples, so each sample is
 * finished a sample later: the function runs a sample ahead (in step with enaga_oscillator_add) and *carry holds
 * the sample in hand between blocks (zero it when the note starts). */
void enaga_oscillator_sync_add(enaga_oscillator *o, float *carry, const enaga_oscillator *master, float dt_m,
                               float inv_dt_m, float *out, int n, int wave, float dt, float inv_dt, float level,
                               float width);

/* ---- Wavetable oscillators: the same phase, reading a table the app fills ---- */

/* A table is 2^bits samples of one cycle (bits 1-24) and a guard sample after them, equal to the first, so reads
 * interpolate linearly without wrapping. Band-limiting is the table's job: an app playing high notes keeps a
 * table per octave with fewer harmonics, say. dt in (-0.5, 0.5): negative runs the table backwards. */
void enaga_wavetable_add(enaga_oscillator *o, const float *table, int bits, float *out, int n, float dt,
                         float level);
/* Two tables of the same size crossfaded: (1 - morph) x a + morph x b, one phase for both. */
void enaga_wavetable_morph_add(enaga_oscillator *o, const float *a, const float *b, int bits, float morph,
                               float *out, int n, float dt, float level);

/* ---- State-variable filter (topology-preserving, trapezoidal) ---- */

/* a1 = 1 / (1 + g (g + k)), a2 = g a1, a3 = g a2, with g = tan(pi cutoff / rate) and k = 1 / Q (2 for none). */
typedef struct { float a1, a2, a3, k; } enaga_svf_coefficients;
typedef struct { float s1, s2; } enaga_svf;

enum { ENAGA_SVF_LOW, ENAGA_SVF_BAND, ENAGA_SVF_HIGH };

/* n sets of coefficients from g[i] and k[i] (see above), as SvfCoefficients::from_g: for many filters at once. */
void enaga_svf_coefficients_buffer(const float *g, const float *k, enaga_svf_coefficients *c, int n);

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
    return 1.0f - enaga_exp(-2.0f * ENAGA_PI * cutoff * enaga_reciprocal(rate));
}
static inline float enaga_one_pole_step(enaga_one_pole *p, float x, float a) { return p->y += a * (x - p->y); }
void enaga_one_pole_process(enaga_one_pole *p, float a, const float *in, float *out, int n);   /* out may be in */

/* ---- DC blocker: y = x - x1 + r y1, a high-pass a few Hz up that takes out any offset ---- */

typedef struct { float x1, y1; } enaga_dc_blocker;
/* r = exp(-2 pi cutoff / rate); 10 Hz is usual (-3 dB there, 0.1 dB down at 50 Hz). */
static inline float enaga_dc_blocker_coefficient(float cutoff, float rate) {
    return enaga_exp(-2.0f * ENAGA_PI * cutoff * enaga_reciprocal(rate));
}
static inline float enaga_dc_blocker_step(enaga_dc_blocker *d, float x, float r) {
    float y = x - d->x1 + r * d->y1;
    d->x1 = x;
    return d->y1 = y;
}
void enaga_dc_blocker_process(enaga_dc_blocker *d, float r, const float *in, float *out, int n);   /* out may be in */

/* ---- Biquads: direct form I, coefficients from the Audio EQ Cookbook (R. Bristow-Johnson) ---- */

/* y = b0 x + b1 x1 + b2 x2 - a2 y2 - a1 y1, normalised so a0 = 1. */
typedef struct { float b0, b1, b2, a1, a2; } enaga_biquad_coefficients;
typedef struct { float x1, x2, y1, y2; } enaga_biquad;

enum {
    ENAGA_BIQUAD_LOW_PASS, ENAGA_BIQUAD_HIGH_PASS, ENAGA_BIQUAD_BAND_PASS,   /* band-pass: 0 dB at the centre */
    ENAGA_BIQUAD_NOTCH, ENAGA_BIQUAD_ALL_PASS, ENAGA_BIQUAD_PEAK, ENAGA_BIQUAD_LOW_SHELF, ENAGA_BIQUAD_HIGH_SHELF
};

/* frequency in Hz (clamped to 0.0001-0.49 of the rate); q > 0 (0.7071 is Butterworth; for the shelves, 0.7071 is
 * the steepest slope without overshoot); gain_db for PEAK and the shelves only. Control rate. */
void enaga_biquad_design(enaga_biquad_coefficients *c, int type, float frequency, float q, float gain_db,
                         float rate);
static inline float enaga_biquad_step(enaga_biquad *s, const enaga_biquad_coefficients *c, float x) {
    float y = c->b0 * x + c->b1 * s->x1 + c->b2 * s->x2 - c->a2 * s->y2 - c->a1 * s->y1;
    s->x2 = s->x1;
    s->x1 = x;
    s->y2 = s->y1;
    s->y1 = y;
    return y;
}
void enaga_biquad_process(enaga_biquad *s, const enaga_biquad_coefficients *c, const float *in, float *out,
                          int n);   /* out may be in */
/* Two biquads in series (a fourth-order filter): the same as two enaga_biquad_process calls, faster, since the two
 * stages overlap. out may be in. */
void enaga_biquad_cascade(enaga_biquad *s1, const enaga_biquad_coefficients *c1, enaga_biquad *s2,
                          const enaga_biquad_coefficients *c2, const float *in, float *out, int n);

/* ---- FIR filters: out[i] = sum of taps[k] x in[i - k], k = 0 to length - 1 ---- */

#define ENAGA_FIR_CHUNK 64

/* The taps are the caller's and must outlive the filter; so is the history memory (enaga_fir_memory floats,
 * zeroed). Fir in filter.hpp allocates it on the app heap. */
typedef struct { const float *taps; float *history; int32_t length; } enaga_fir;

static inline uint32_t enaga_fir_memory(int length) { return (uint32_t)(length - 1 + ENAGA_FIR_CHUNK); }
static inline void enaga_fir_init(enaga_fir *f, const float *taps, int length, float *memory) {
    f->taps = taps;
    f->history = memory;
    f->length = length;
}
/* Sums in the order k = 0, 1, ... for every output, as the plain loop would. out may be in. */
void enaga_fir_process(enaga_fir *f, const float *in, float *out, int n);

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
/* A modulated read (chorus, flanger, vibrato): out[i] = tap_linear(delay[i]) at sample i of the block, as
 * enaga_delay_read, provided every delay[i] >= i (n - 1 or more always does) and at most the capacity. */
void enaga_delay_read_linear(const enaga_delay_line *d, const float *delay, float *out, int n);

/* ---- Stereo delay: feedback through a low-pass (tone), optional ping-pong, a gliding time ---- */

typedef struct {
    enaga_delay_line left, right;
    enaga_one_pole damp_l, damp_r;
    float rate, current, target, feedback, tone;    /* current and target: the delay in samples */
    int32_t ping_pong;
} enaga_stereo_delay;

/* The floats of memory enaga_stereo_delay_init needs (zeroed) for delays up to max_seconds. */
uint32_t enaga_stereo_delay_memory(float max_seconds, float rate);
/* The time starts at 0.25 s, or max_seconds if that is shorter. */
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

/* ---- Chorus: two lines, each read at a delay swept by a sine, the two a quarter turn apart ---- */

typedef struct {
    enaga_delay_line left, right;
    uint32_t phase, step;           /* the sweep: a 32-bit fraction of a turn, and its step per sample */
    float base, depth, rate;        /* base and depth in samples */
} enaga_chorus;

/* The floats of memory enaga_chorus_init needs (zeroed) at this sample rate. */
uint32_t enaga_chorus_memory(float rate);
void enaga_chorus_init(enaga_chorus *c, float *memory, float rate);
/* sweep_hz: the sweep's rate; depth 0-1, up to +-5 ms around an 8 ms delay (below about 22 kHz, less: the
 * delay stays 66 samples or more). */
void enaga_chorus_set(enaga_chorus *c, float sweep_hz, float depth);
/* Adds mix x the chorused in_l / in_r to out_l / out_r. out may be in (in place). */
void enaga_chorus_process(enaga_chorus *c, const float *in_l, const float *in_r, float *out_l, float *out_r, int n,
                          float mix);

/* ---- Bitcrusher: fewer bits and a lower sample rate, with no filtering (the aliasing is the point) ---- */

typedef struct {
    float step, inv_step;           /* the quantiser's step, and 1 / step */
    uint32_t increment, phase;      /* the held rate over the sample rate, and the way to the next sample taken:
                                     * fractions of 2^31 */
    float held_l, held_r;
} enaga_crusher;

void enaga_crusher_init(enaga_crusher *c);           /* 24 bits at 48 kHz: as good as untouched */
/* bits 1-24, fractions allowed (2^bits levels across -1 to 1; 24 is as good as untouched); rate_hz the rate
 * samples are taken at, up to sample_rate (each is held until the next). */
void enaga_crusher_set(enaga_crusher *c, float bits, float rate_hz, float sample_rate);
/* In place; right may be NULL (mono), or another buffer than left. */
void enaga_crusher_process(enaga_crusher *c, float *left, float *right, int n);

/* ---- Compressor: feed-forward, stereo linked, peak detection, a soft knee, attack and release in dB ---- */

typedef struct {
    float threshold, slope, half_knee, knee_scale;   /* dB; slope = 1 - 1 / ratio; knee_scale = slope / (2 knee) */
    float attack, release;          /* one-pole coefficients for the gain reduction */
    float makeup;                   /* dB */
    float reduction;                /* the gain reduction now, dB (0 or more) */
    float rate;
} enaga_compressor;

/* Starts at -12 dB, 4:1, 5 ms attack, 100 ms release, a 6 dB knee, no makeup. */
void enaga_compressor_init(enaga_compressor *c, float rate);
/* threshold_db (0 and below); ratio 1 or more (20 or more is limiting); attack_s and release_s time constants;
 * knee_db the width of the soft knee around the threshold (0 hard); makeup_db added after. */
void enaga_compressor_set(enaga_compressor *c, float threshold_db, float ratio, float attack_s, float release_s,
                          float knee_db, float makeup_db);
/* Compresses left and right (in place; right may be NULL for mono) by the level of key_l / key_r, the side
 * chain, or of the signal itself where key_l is NULL (key_r NULL with key_l given: a mono key). */
void enaga_compressor_process(enaga_compressor *c, float *left, float *right, int n, const float *key_l,
                              const float *key_r);

/* ---- Limiter: look-ahead, so no sample leaves above the ceiling ---- */

/* The gain each sample needs is held over the look-ahead and faded in over it, ahead of the delayed signal, so it
 * is down by the time the peak arrives; it recovers with the release. The signal comes out
 * enaga_limiter_latency samples late. */
typedef struct {
    float *delay_l, *delay_r, *box, *blocks;  /* box: the gains - 1 of the last `length` samples */
    uint32_t mask, length, write;   /* length: the look-ahead in samples, the fade's length */
    int32_t block_count, block, in_block;
    float partial, window, depth, sum, inv_length;   /* depth: the released reduction, 1 - gain */
    float ceiling, release, rate;
    float lowest;                   /* the lowest gain applied in the last process call */
} enaga_limiter;

/* The floats of memory enaga_limiter_init needs (zeroed) for a look-ahead of lookahead_s: 1-5 ms is usual; 16
 * to 4096 samples. It starts at a 0 dB ceiling and a 0.1 s release. */
uint32_t enaga_limiter_memory(float lookahead_s, float rate);
void enaga_limiter_init(enaga_limiter *l, float *memory, float lookahead_s, float rate);
/* ceiling_db: no sample leaves above it (-0.3 dB leaves room for conversion); release_s a time constant. */
void enaga_limiter_set(enaga_limiter *l, float ceiling_db, float release_s);
static inline uint32_t enaga_limiter_latency(const enaga_limiter *l) { return l->length - 1; }
/* In place; right may be NULL (mono). */
void enaga_limiter_process(enaga_limiter *l, float *left, float *right, int n);

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

/* ---- Parameter smoothing: a one-pole glide towards a target, so knob changes do not click ---- */

typedef struct {
    float value, target;
    float a;                                 /* the per-step coefficient: enaga_smoother_coefficient */
} enaga_smoother;

/* The coefficient for gliding to within 1 % of a new target in `seconds`, stepped `rate` times a second. */
static inline float enaga_smoother_coefficient(float seconds, float rate) {
    float steps = seconds * rate;
    return steps < 1.0f ? 1.0f : 1.0f - enaga_exp(-4.60517019f * enaga_reciprocal(steps));
}
/* One step; returns the new value. A step too small to change the value (under half its last bit) lands on the
 * target, so the glide ends there. */
static inline float enaga_smoother_step(enaga_smoother *s) {
    float v = s->value + s->a * (s->target - s->value);
    return s->value = v == s->value ? s->target : v;
}
/* n steps into out: the same values as n calls of enaga_smoother_step. */
void enaga_smoother_render(enaga_smoother *s, float *out, int n);

/* ---- Sample formats: WAV sample data to float or int16, and back ---- */

/* The sample formats of WAV files (<enaga/wav.hpp>): the value is the bits per sample, 33 for 32-bit float. */
enum { ENAGA_PCM_U8 = 8, ENAGA_PCM_S16 = 16, ENAGA_PCM_S24 = 24, ENAGA_PCM_S32 = 32, ENAGA_PCM_FLOAT = 33 };

/* The format of a WAV's samples from its header's encoding (1 PCM, 3 float) and bits, or -1 if unknown. */
static inline int enaga_pcm_format(int encoding, int bits) {
    if (encoding == 1) return bits == 8 || bits == 16 || bits == 24 || bits == 32 ? bits : -1;
    return encoding == 3 && bits == 32 ? ENAGA_PCM_FLOAT : -1;
}
static inline int enaga_pcm_bytes(int format) { return format == ENAGA_PCM_FLOAT ? 4 : format / 8; }

/* n samples of `format` from src, little-endian at any alignment (read byte by byte), every stride-th sample:
 * stride 1 for mono or for interleaved samples kept interleaved, or the channel count with src at a channel's
 * first sample to take that channel out. Floats are full scale at ±1 (infinity and NaN read as 0); 8-bit WAV is
 * unsigned. */
void enaga_pcm_to_float(const void *src, int format, int stride, float *out, int n);
/* The same into int16 (half the memory of float), rounded to nearest and saturated. */
void enaga_pcm_to_int16(const void *src, int format, int stride, int16_t *out, int n);
/* out = gain x in / 32768: int16 samples back to float, for playing them. */
void enaga_int16_to_float(const int16_t *in, float *out, int n, float gain);
/* out = in x 32768, rounded to nearest and saturated: float to 16-bit, for writing a 16-bit WAV. */
void enaga_float_to_int16(const float *in, int16_t *out, int n);

/* ---- Rate conversion: windowed sinc, a fixed ratio, block by block ---- */

/* A 64-tap Kaiser-windowed sinc in 64 phases, linearly interpolated between them: flat to 0.86 of the lower
 * rate's Nyquist (19 kHz from 44.1 kHz), and what would alias into the band below that is about 89 dB down. The
 * table (16.6 KB) depends only on the two rates: one serves every channel converting between them. Each output
 * costs about 128 multiply-adds, so convert when a sample is loaded, not per voice in the audio callback (to play
 * at a pitch, interpolate). */
#define ENAGA_RESAMPLE_TAPS 64
#define ENAGA_RESAMPLE_PHASES 64

static inline uint32_t enaga_resample_table_size(void) {             /* floats */
    return (ENAGA_RESAMPLE_PHASES + 1) * ENAGA_RESAMPLE_TAPS;
}
/* Fills table (enaga_resample_table_size() floats) with the filter for in_rate to out_rate (Hz, 1 to 1,000,000):
 * cut off at 0.95 of the lower rate's Nyquist, so going down removes what would alias. */
void enaga_resample_table(float *table, uint32_t in_rate, uint32_t out_rate);

typedef struct {
    const float *table;
    uint32_t step_int, step_frac;   /* in_rate / out_rate in 32.32 fixed point */
    uint32_t frac;                  /* where the next output lies after the current input, a 32-bit fraction */
    uint32_t skip;                  /* inputs to take before the next output */
    uint32_t write;                 /* window position */
    float window[2 * ENAGA_RESAMPLE_TAPS];   /* the last TAPS inputs, written twice so they are always in a row */
} enaga_resampler;

/* Starts a conversion from the beginning of a sound, with a table made for the same rates. */
void enaga_resampler_init(enaga_resampler *r, const float *table, uint32_t in_rate, uint32_t out_rate);
/* Converts up to n_in samples from in into out, at most max_out of them; returns how many it wrote and sets
 * *used to how many inputs it took. If it filled out, call again, with the inputs not yet used (none, if all
 * were): outputs may be waiting even when every input was taken. Output 0 is
 * input 0's instant, with no delay; each output needs ENAGA_RESAMPLE_TAPS / 2 inputs after it, so at the end of
 * a sound feed that many zeros to get the last outputs. Mono: one resampler per channel. */
int enaga_resample(enaga_resampler *r, const float *in, int n_in, float *out, int max_out, int *used);

/* ---- Samples in memory and their player ---- */

/* A sample: `frames` frames of float at `rate` Hz, one channel, or two interleaved (L R L R). */
typedef struct {
    float *data;
    uint32_t frames;
    uint32_t channels;                       /* 1 or 2 */
    uint32_t rate;
} enaga_sample;

enum { ENAGA_LOOP_OFF = 0, ENAGA_LOOP_FORWARD = 1, ENAGA_LOOP_PING_PONG = 2 };

/* Plays a sample from `start` to `end` (frames, end excluded), forwards or in reverse, looping between
 * `loop_start` and `loop_end` once it gets there if `loop` says so. The position is 32.32 fixed point in frames,
 * read by 4-point cubic (Catmull-Rom) interpolation; frames outside the sample read as its first or last, and in a
 * forward loop those past the loop's ends as the frames at its other end. The fields may be changed between calls:
 * `loop` set to ENAGA_LOOP_OFF lets a looping note play out to its end. */
typedef struct {
    const enaga_sample *sample;              /* must outlive the playing */
    int64_t position;                        /* frames << 32 */
    uint32_t start, end, loop_start, loop_end;
    int32_t loop;                            /* ENAGA_LOOP_OFF, _FORWARD, _PING_PONG */
    int32_t direction;                       /* +1, or -1 in reverse (and on a ping-pong's way back) */
    int32_t active;
    int32_t fresh;                           /* render_add takes its first gains at once, then ramps */
    float gain_l, gain_r;                    /* render_add's gains at the end of the last block */
} enaga_sample_player;

/* Starts at `start` (or at end - 1 in reverse), with no loop; the whole sample if end is 0 or past its end. */
void enaga_sample_player_start(enaga_sample_player *p, const enaga_sample *s, uint32_t start, uint32_t end, int reverse);
/* Writes up to n frames, advancing `rate` source frames per output frame (any rate >= 0: the pitch ratio times
 * the sample's rate over the output's): a mono sample into left only (right may be NULL), a stereo one into both.
 * After the end, the rest of the block is zeros. Returns the frames played. */
int enaga_sample_player_render(enaga_sample_player *p, float *left, float *right, int n, float rate);
/* Adds up to n frames into left and right, a mono sample into both, with gains moving in a straight line from
 * the last call's to these across the block (the first block after start takes them at once). The frames played. */
int enaga_sample_player_render_add(enaga_sample_player *p, float *left, float *right, int n, float rate, float gain_l,
                                   float gain_r);

#ifdef __cplusplus
}
#endif

#endif
