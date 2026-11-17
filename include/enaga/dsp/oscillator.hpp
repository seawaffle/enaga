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
    float phase() const { return phase_; }
    void reset(float phase = 0.0f) { phase_ = phase; }

    // Adds level x the waveform to out[0..n), advancing the phase. dt in (0, 0.5); pulse width 0.05-0.95.
    void render_add(float *out, int n, Wave wave, float dt, float inv_dt, float level, float width = 0.5f) {
        float t = phase_;
        switch (wave) {
        case Wave::saw:
            for (int i = 0; i < n; i++) {
                out[i] += level * (2.0f * t - 1.0f - blep(t, dt, inv_dt));
                t += dt;
                if (t >= 1.0f) t -= 1.0f;
            }
            break;
        case Wave::pulse:
            width = clamp(width, 0.05f, 0.95f);
            for (int i = 0; i < n; i++) {
                float t2 = t + 1.0f - width;
                if (t2 >= 1.0f) t2 -= 1.0f;
                float y = (t < width ? 1.0f : -1.0f) + blep(t, dt, inv_dt) - blep(t2, dt, inv_dt);
                out[i] += level * (y + 1.0f - 2.0f * width);   // DC removed, so width changes don't thump
                t += dt;
                if (t >= 1.0f) t -= 1.0f;
            }
            break;
        case Wave::triangle:
            for (int i = 0; i < n; i++) {
                float y = 4.0f * t - 1.0f;
                out[i] += level * (t < 0.5f ? y : 2.0f - y);
                t += dt;
                if (t >= 1.0f) t -= 1.0f;
            }
            break;
        case Wave::sine:
            for (int i = 0; i < n; i++) {
                out[i] += level * sin_turns(t);
                t += dt;
                if (t >= 1.0f) t -= 1.0f;
            }
            break;
        }
        phase_ = t;
    }

private:
    // The PolyBLEP residual at phase t: a step of +2 at t = 0 smoothed over one sample either side.
    static float blep(float t, float dt, float inv_dt) {
        if (t < dt) {
            t *= inv_dt;
            return t + t - t * t - 1.0f;
        }
        if (t > 1.0f - dt) {
            t = (t - 1.0f) * inv_dt;
            return t * t + t + t + 1.0f;
        }
        return 0.0f;
    }

    float phase_ = 0.0f;
};

}  // namespace enaga::dsp
