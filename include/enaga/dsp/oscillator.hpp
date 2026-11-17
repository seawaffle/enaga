// Enaga DSP: band-limited oscillators. Provisional until 1.0.
//
// Saw and pulse are PolyBLEP-corrected (aliasing well down at the pitches of a keyboard instrument);
// triangle and sine are naive, their harmonics falling fast enough. The frequency is given per call as dt, the
// cycles per sample, with its reciprocal (the caller computes 1 / dt at control rate so nothing here divides).
#pragma once

#include <enaga/dsp/math.hpp>

namespace enaga::dsp {

enum class Wave : uint8_t { saw, pulse, triangle, sine };

class Oscillator {
public:
    float phase() const { return enaga_oscillator_phase(&state_); }
    void reset(float phase = 0.0f) { enaga_oscillator_reset(&state_, phase); }

    // Adds level x the waveform to out[0..n), advancing the phase. dt in (0, 0.5); pulse width 0.05-0.95.
    // A block function (enaga_oscillator_add).
    void render_add(float *out, int n, Wave wave, float dt, float inv_dt, float level, float width = 0.5f) {
        enaga_oscillator_add(&state_, out, n, int(wave), dt, inv_dt, level, width);
    }

    enaga_oscillator state_{};     // public, for C code working on the same oscillator
};

static_assert(int(Wave::saw) == ENAGA_WAVE_SAW && int(Wave::pulse) == ENAGA_WAVE_PULSE &&
              int(Wave::triangle) == ENAGA_WAVE_TRIANGLE && int(Wave::sine) == ENAGA_WAVE_SINE);

}  // namespace enaga::dsp
