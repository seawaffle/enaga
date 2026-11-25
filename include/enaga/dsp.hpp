// Enaga DSP library: building blocks for instruments. Provisional until 1.0 (docs/design.md,
// "What a developer gets"). Everything works in float at any sample rate; the audio callback runs at
// ENAGA_SAMPLE_RATE in blocks of ENAGA_BLOCK_FRAMES.
//
// The classes wrap the C interface in <enaga/dsp.h>: their state is its plain structs, and their block methods
// (render_add, process, render) are its block functions, which the app build compiles with TI's cl6x when it is
// installed. Process in blocks; per-sample step() calls are for code that needs them.
//
//   math.hpp        exp2, log2, pow, sqrt, sin, tanh, atan2, decibels, ..., Random, Memory: fast, no division
//   oscillator.hpp  PolyBLEP saw and pulse, triangle, sine, hard sync; wavetables and a morph between two
//   filter.hpp      state-variable filter (low, band, high) and a two-stage cascade, one-pole, DC blocker,
//                   biquads, FIR
//   modulation.hpp  ADSR envelope, LFO, parameter smoothing (Smoothed)
//   effects.hpp     delay line, stereo delay, chorus, reverb (buffers on the app heap)
//   dynamics.hpp    compressor (with a side chain), look-ahead limiter, bitcrusher
//   sample.hpp      samples in memory and their player; rate conversion (Resampler); the sample format
//                   functions are the C ones in dsp.h
//   voices.hpp      polyphony: which voice plays a note (VoiceAllocator), the keys held (NoteStack)
//
// docs/api.md, "DSP library", is the reference.
#pragma once

#include <enaga/dsp/dynamics.hpp>
#include <enaga/dsp/effects.hpp>
#include <enaga/dsp/filter.hpp>
#include <enaga/dsp/math.hpp>
#include <enaga/dsp/modulation.hpp>
#include <enaga/dsp/oscillator.hpp>
#include <enaga/dsp/sample.hpp>
#include <enaga/dsp/voices.hpp>
