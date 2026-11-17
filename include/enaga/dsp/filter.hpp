// Enaga DSP: filters. Provisional until 1.0.
//
// Svf is the topology-preserving (trapezoidal) state-variable filter: stable under fast modulation, with
// low-pass, band-pass and high-pass at once. Coefficients cost one division, so compute them at control rate
// (SvfCoefficients) and run many samples with them: Svf::process, a block function (include/enaga/dsp.h).
#pragma once

#include <enaga/dsp/math.hpp>

namespace enaga::dsp {

struct SvfCoefficients : enaga_svf_coefficients {
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
    enum Output { low_pass = ENAGA_SVF_LOW, band_pass = ENAGA_SVF_BAND, high_pass = ENAGA_SVF_HIGH };

    void reset() { state_ = {}; }

    // One sample.
    Out step(float x, const SvfCoefficients &c) {
        Out o;
        enaga_svf_step(&state_, &c, x, &o.low, &o.band, &o.high);
        return o;
    }
    float low(float x, const SvfCoefficients &c) { return step(x, c).low; }

    // A block: out = one output of in (out may be in). A block function (enaga_svf_process): prefer it to step().
    void process(const float *in, float *out, int n, const SvfCoefficients &c, Output output = low_pass) {
        enaga_svf_process(&state_, &c, output, in, out, n);
    }

    // Two filters in series, both giving `output`: the same as a.process then b.process, about twice as fast.
    static void cascade(Svf &a, const SvfCoefficients &ca, Svf &b, const SvfCoefficients &cb, const float *in,
                        float *out, int n, Output output = low_pass) {
        enaga_svf_cascade(&a.state_, &ca, &b.state_, &cb, output, in, out, n);
    }

    enaga_svf state_{};     // public, for C code working on the same filter
};

// A one-pole low-pass, for damping and smoothing: y += a (x - y), a = 1 - exp(-2 pi f / rate).
class OnePole {
public:
    static float coefficient(float cutoff, float sample_rate) {
        return 1.0f - exp(-2.0f * pi * cutoff / sample_rate);
    }
    float step(float x, float a) { return enaga_one_pole_step(&state_, x, a); }
    // A block (out may be in): enaga_one_pole_process.
    void process(const float *in, float *out, int n, float a) { enaga_one_pole_process(&state_, a, in, out, n); }
    float value() const { return state_.y; }
    void reset(float y = 0.0f) { state_.y = y; }

    enaga_one_pole state_{};
};

}  // namespace enaga::dsp
