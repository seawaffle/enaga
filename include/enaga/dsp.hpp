// Enaga DSP library: building blocks for instruments. Provisional until 1.0 (docs/design.md,
// "What a developer gets"). Everything works in float at any sample rate; the audio callback runs at
// ENAGA_SAMPLE_RATE in blocks of ENAGA_BLOCK_FRAMES.
//
// The classes wrap the C interface in <enaga/dsp.h>: their state is its plain structs, and their block methods
// (render_add, process, render) are its block functions, which the app build compiles with TI's cl6x when it is
// installed. Process in blocks; per-sample step() calls are for code that needs them.
//
//   math.hpp        exp2, semitones, sin_turns, tan_pi, soft_clip, Random: fast, no libm, no division
//   oscillator.hpp  PolyBLEP saw and pulse, triangle, sine
//   filter.hpp      state-variable filter (low, band, high) and a two-stage cascade, one-pole
//   modulation.hpp  ADSR envelope, LFO
//   effects.hpp     delay line, stereo delay, reverb (buffers on the app heap)
#pragma once

#include <enaga/dsp/effects.hpp>
#include <enaga/dsp/filter.hpp>
#include <enaga/dsp/math.hpp>
#include <enaga/dsp/modulation.hpp>
#include <enaga/dsp/oscillator.hpp>
