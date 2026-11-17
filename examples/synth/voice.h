/* The voice's inner loops (oscillators, noise, filter), in C so that cl6x can build them (voice.ti.c;
 * tools/build_app.py). Each mirrors the enaga::dsp code named beside it. */
#ifndef SYNTH_VOICE_H
#define SYNTH_VOICE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The same fields and maths as enaga::dsp::SvfCoefficients and Svf. */
typedef struct { float a1, a2, a3, k; } SynthSvfCoefficients;
typedef struct { float s1, s2; } SynthSvfState;

enum { SYNTH_LP12, SYNTH_LP24, SYNTH_BP, SYNTH_HP };   /* the order of synth::FilterType */

/* buf[i] = filter(soft_clip(drive * buf[i])) for i < n. LP24 runs c1/f1 then c2/f2; the others c2/f2 only. */
void synth_filter(float *buf, int n, int type, float drive, const SynthSvfCoefficients *c1,
                  const SynthSvfCoefficients *c2, SynthSvfState *f1, SynthSvfState *f2);

enum { SYNTH_SAW, SYNTH_PULSE, SYNTH_TRIANGLE, SYNTH_SINE };   /* the order of enaga::dsp::Wave */

/* enaga::dsp::Oscillator::render_add: adds level x the wave to out[0..n) from phase t; returns the new phase. */
float synth_oscillator(float *out, int n, int wave, float t, float dt, float inv_dt, float level, float width);

/* Adds level x white noise (xorshift32, as enaga::dsp::Random::bipolar) to out[0..n). */
void synth_noise(float *out, int n, float level, uint32_t *state);

#ifdef __cplusplus
}
#endif

#endif
