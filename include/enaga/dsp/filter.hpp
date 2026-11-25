// Enaga DSP: filters. Provisional until 1.0.
//
// Svf is the topology-preserving (trapezoidal) state-variable filter: stable under fast modulation, with
// low-pass, band-pass and high-pass at once. Coefficients cost a reciprocal, so compute them at control rate
// (SvfCoefficients) and run many samples with them: Svf::process, a block function (include/enaga/dsp.h).
// Biquad covers the cookbook shapes (shelves, peaks, notches, all-passes) for EQ; Fir runs a kernel the app
// supplies. OnePole smooths; DcBlocker takes out offsets.
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
        c.a1 = reciprocal(1.0f + g * (g + k));
        c.a2 = g * c.a1;
        c.a3 = g * c.a2;
        return c;
    }
};

// The same layout as the C struct, so an array of either serves enaga_svf_coefficients_buffer.
static_assert(sizeof(SvfCoefficients) == sizeof(enaga_svf_coefficients));

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
        return enaga_one_pole_coefficient(cutoff, sample_rate);
    }
    float step(float x, float a) { return enaga_one_pole_step(&state_, x, a); }
    // A block (out may be in): enaga_one_pole_process.
    void process(const float *in, float *out, int n, float a) { enaga_one_pole_process(&state_, a, in, out, n); }
    float value() const { return state_.y; }
    void reset(float y = 0.0f) { state_.y = y; }

    enaga_one_pole state_{};
};

// A DC blocker: a high-pass a few hertz up that takes out any offset (after a waveshaper, a resonant filter or a
// recording), y = x - x1 + r y1.
class DcBlocker {
public:
    explicit DcBlocker(float cutoff = 10.0f, float sample_rate = 48000.0f) { set(cutoff, sample_rate); }
    void set(float cutoff, float sample_rate) { r_ = enaga_dc_blocker_coefficient(cutoff, sample_rate); }
    float step(float x) { return enaga_dc_blocker_step(&state_, x, r_); }
    // A block (out may be in): enaga_dc_blocker_process.
    void process(const float *in, float *out, int n) { enaga_dc_blocker_process(&state_, r_, in, out, n); }
    void reset() { state_ = {}; }

    enaga_dc_blocker state_{};

private:
    float r_ = 0.0f;
};

struct BiquadCoefficients : enaga_biquad_coefficients {
    enum Type {
        low_pass = ENAGA_BIQUAD_LOW_PASS, high_pass = ENAGA_BIQUAD_HIGH_PASS, band_pass = ENAGA_BIQUAD_BAND_PASS,
        notch = ENAGA_BIQUAD_NOTCH, all_pass = ENAGA_BIQUAD_ALL_PASS, peak = ENAGA_BIQUAD_PEAK,
        low_shelf = ENAGA_BIQUAD_LOW_SHELF, high_shelf = ENAGA_BIQUAD_HIGH_SHELF
    };
    // frequency in Hz; q > 0 (0.7071 Butterworth); gain_db for peak and the shelves. Control rate.
    static BiquadCoefficients make(Type type, float frequency, float q, float sample_rate, float gain_db = 0.0f) {
        BiquadCoefficients c;
        enaga_biquad_design(&c, type, frequency, q, gain_db, sample_rate);
        return c;
    }
};

class Biquad {
public:
    void reset() { state_ = {}; }
    float step(float x, const BiquadCoefficients &c) { return enaga_biquad_step(&state_, &c, x); }
    // A block (out may be in): enaga_biquad_process.
    void process(const float *in, float *out, int n, const BiquadCoefficients &c) {
        enaga_biquad_process(&state_, &c, in, out, n);
    }
    // Two in series: the same as a.process then b.process, faster.
    static void cascade(Biquad &a, const BiquadCoefficients &ca, Biquad &b, const BiquadCoefficients &cb,
                        const float *in, float *out, int n) {
        enaga_biquad_cascade(&a.state_, &ca, &b.state_, &cb, in, out, n);
    }

    enaga_biquad state_{};
};

// An FIR filter on the app's taps (which must outlive it): out[i] = sum of taps[k] in[i - k]. The history comes
// from the app heap: call init() in the app's init(), not in the audio callback.
class Fir {
public:
    void init(const float *taps, int length) {
        enaga_fir_init(&state_, taps, length, memory_.allocate(enaga_fir_memory(length)));
    }
    void process(const float *in, float *out, int n) { enaga_fir_process(&state_, in, out, n); }   // out may be in

    enaga_fir state_{};

private:
    Memory memory_;
};

static_assert(int(BiquadCoefficients::high_shelf) == ENAGA_BIQUAD_HIGH_SHELF);

}  // namespace enaga::dsp
