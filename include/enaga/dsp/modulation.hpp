// Enaga DSP: envelopes and LFOs. Provisional until 1.0.
//
// Both run at whatever rate the caller steps them: per sample for an amplitude envelope, once per control block
// for modulation. Their settings (EnvelopeShape, the LFO rate) are computed for that rate, once per change.
#pragma once

#include <enaga/dsp/math.hpp>

namespace enaga::dsp {

// ADSR settings as one-pole coefficients for a given step rate.
struct EnvelopeShape : enaga_envelope_shape {

    // Times in seconds (attack to full level, decay and release to within 1% of their target), sustain 0-1.
    static EnvelopeShape make(float attack_s, float decay_s, float sustain, float release_s, float step_rate) {
        return {{coefficient(attack_s, step_rate, kAttackSpan), coefficient(decay_s, step_rate, kSpan),
                 clamp(sustain, 0.0f, 1.0f), coefficient(release_s, step_rate, kSpan)}};
    }

    // The attack heads for kAttackTarget and stops at 1: a curve like an analogue envelope's.
    static constexpr float kAttackTarget = ENAGA_ENV_ATTACK_TARGET;
    static constexpr float kAttackSpan = 1.46633707f;    // ln(1.3 / 0.3): steps to cover 1.3 x (1 - e^-span) = 1
    static constexpr float kSpan = 4.60517019f;          // ln(100)

private:
    static float coefficient(float seconds, float rate, float span) {
        float steps = seconds * rate;
        return steps < 1.0f ? 1.0f : 1.0f - exp(-span / steps);
    }
};

class Envelope {
public:
    enum Stage : uint8_t {
        idle = ENAGA_ENV_IDLE, attack = ENAGA_ENV_ATTACK, decay = ENAGA_ENV_DECAY, sustain = ENAGA_ENV_SUSTAIN,
        release = ENAGA_ENV_RELEASE
    };

    void gate_on(bool retrigger_from_zero = false) {
        if (retrigger_from_zero) state_.level = 0.0f;
        state_.stage = attack;
    }
    void gate_off() { if (state_.stage != idle) state_.stage = release; }
    void kill() { state_.stage = idle; state_.level = 0.0f; }
    Stage stage() const { return Stage(state_.stage); }
    bool active() const { return state_.stage != idle; }
    float level() const { return state_.level; }

    // One step; returns the level.
    float step(const EnvelopeShape &s) { return enaga_envelope_step(&state_, &s); }
    // n steps into out (the same values as n calls of step): a block function (enaga_envelope_render).
    void render(float *out, int n, const EnvelopeShape &s) { enaga_envelope_render(&state_, &s, out, n); }

    enaga_envelope state_{};     // public, for C code working on the same envelope
};

enum class LfoShape : uint8_t { sine, triangle, saw, square, sample_hold };

// A bipolar LFO, -1 to 1. Step it with dt = rate / step rate.
class Lfo {
public:
    void reset(float phase = 0.0f) { phase_ = phase; }
    float phase() const { return phase_; }

    float step(LfoShape shape, float dt, Random &random) {
        phase_ += dt;
        if (phase_ >= 1.0f) {
            phase_ -= float(floor_int(phase_));
            held_ = random.bipolar();
        }
        return value(shape);
    }
    float value(LfoShape shape) const {
        switch (shape) {
        case LfoShape::sine: return sin_turns(phase_);
        case LfoShape::triangle: return phase_ < 0.5f ? 4.0f * phase_ - 1.0f : 3.0f - 4.0f * phase_;
        case LfoShape::saw: return 2.0f * phase_ - 1.0f;
        case LfoShape::square: return phase_ < 0.5f ? 1.0f : -1.0f;
        case LfoShape::sample_hold: return held_;
        }
        return 0.0f;
    }

private:
    float phase_ = 0.0f, held_ = 0.0f;
};

// A parameter that glides to each new value instead of jumping, so knob moves do not click or zipper: one-pole,
// reaching within 1 % of a new target in the time set. Step it per sample (next, render), or once per block for
// a coefficient computed per block (advance).
class Smoothed {
public:
    // Glide time in seconds, at `rate` steps a second, starting at `value` with no glide.
    void init(float seconds, float rate, float value = 0.0f, int block = 64) {
        state_ = {value, value, enaga_smoother_coefficient(seconds, rate)};
        block_ = block;
        block_factor_ = 1.0f;
        for (int i = 0; i < block; i++) block_factor_ *= 1.0f - state_.a;
    }
    void set(float target) { state_.target = target; }                   // glides there
    void jump(float value) { state_.value = state_.target = value; }     // at once (a new note, a preset)
    float next() { return enaga_smoother_step(&state_); }                // one step
    void render(float *out, int n) { enaga_smoother_render(&state_, out, n); }   // n steps (a block function)
    // Moves on a whole block (init's `block` steps) at once, exactly as that many next() calls would but for
    // rounding, and returns the value: for settings recomputed once per block.
    float advance() { return state_.value = state_.target + (state_.value - state_.target) * block_factor_; }
    float value() const { return state_.value; }
    float target() const { return state_.target; }
    bool settled(float tolerance = 1e-5f) const { return abs(state_.value - state_.target) <= tolerance; }

    enaga_smoother state_{};

private:
    int block_ = 64;
    float block_factor_ = 1.0f;
};

}  // namespace enaga::dsp

