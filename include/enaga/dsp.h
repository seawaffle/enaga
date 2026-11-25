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
/* sweep_hz: the sweep's rate; depth 0-1, up to +-5 ms around an 8 ms delay. */
void enaga_chorus_set(enaga_chorus *c, float sweep_hz, float depth);
/* Adds mix x the chorused in_l / in_r to out_l / out_r. out may be in (in place). */
void enaga_chorus_process(enaga_chorus *c, const float *in_l, const float *in_r, float *out_l, float *out_r, int n,
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
 * first sample to take that channel out. Floats are full scale at ±1; 8-bit WAV is unsigned. */
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

#ifdef __cplusplus
}
#endif

#endif
