// Enaga DSP: filters. Provisional until 1.0.
//
// Svf is the topology-preserving (trapezoidal) state-variable filter: stable under fast modulation, with
// low-pass, band-pass and high-pass at once. Coefficients cost one division, so compute them at control rate
// (SvfCoefficients) and run many samples with them.
#pragma once

#include <enaga/dsp/math.hpp>

namespace enaga::dsp {

struct SvfCoefficients {
    float a1, a2, a3, k;

    // cutoff in Hz (clamped below Nyquist), resonance 0 (none) to 1 (just short of self-oscillation).
    static SvfCoefficients make(float cutoff, float resonance, float sample_rate) {
        return from_g(tan_pi(cutoff / sample_rate), 2.0f - 1.98f * clamp(resonance, 0.0f, 1.0f));
    }
    // g = tan(pi cutoff / sample rate); k = 1 / Q, 2 for none down towards 0.
    static SvfCoefficients from_g(float g, float k) {
        SvfCoefficients c;
        c.k = k;
        c.a1 = 1.0f / (1.0f + g * (g + k));
        c.a2 = g * c.a1;
        c.a3 = g * c.a2;
        return c;
    }
};

class Svf {
public:
    struct Out { float low, band, high; };

    void reset() { s1_ = s2_ = 0.0f; }

    Out step(float x, const SvfCoefficients &c) {
        float v3 = x - s2_;
        float v1 = c.a1 * s1_ + c.a2 * v3;
        float v2 = s2_ + c.a2 * s1_ + c.a3 * v3;
        s1_ = 2.0f * v1 - s1_;
        s2_ = 2.0f * v2 - s2_;
        return {v2, v1, x - c.k * v1 - v2};
    }

    float low(float x, const SvfCoefficients &c) { return step(x, c).low; }

private:
    float s1_ = 0.0f, s2_ = 0.0f;
};

// A one-pole low-pass, for damping and smoothing: y += a (x - y), a = 1 - exp(-2 pi f / rate).
class OnePole {
public:
    static float coefficient(float cutoff, float sample_rate) {
        return 1.0f - exp(-2.0f * pi * cutoff / sample_rate);
    }
    float step(float x, float a) { return y_ += a * (x - y_); }
    float value() const { return y_; }
    void reset(float y = 0.0f) { y_ = y; }

private:
    float y_ = 0.0f;
};

}  // namespace enaga::dsp
