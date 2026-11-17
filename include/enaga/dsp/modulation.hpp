// Enaga DSP: envelopes and LFOs. Provisional until 1.0.
//
// Both run at whatever rate the caller steps them: per sample for an amplitude envelope, once per control block
// for modulation. Their settings (EnvelopeShape, the LFO rate) are computed for that rate, once per change.
#pragma once

#include <enaga/dsp/math.hpp>

namespace enaga::dsp {

// ADSR settings as one-pole coefficients for a given step rate.
struct EnvelopeShape {
    float attack, decay, sustain, release;

    // Times in seconds (attack to full level, decay and release to within 1% of their target), sustain 0-1.
    static EnvelopeShape make(float attack_s, float decay_s, float sustain, float release_s, float step_rate) {
        return {coefficient(attack_s, step_rate, kAttackSpan), coefficient(decay_s, step_rate, kSpan),
                clamp(sustain, 0.0f, 1.0f), coefficient(release_s, step_rate, kSpan)};
    }

    // The attack heads for kAttackTarget and stops at 1: a curve like an analogue envelope's.
    static constexpr float kAttackTarget = 1.3f;
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
    enum Stage : uint8_t { idle, attack, decay, sustain, release };

    void gate_on(bool retrigger_from_zero = false) {
        if (retrigger_from_zero) level_ = 0.0f;
        stage_ = attack;
    }
    void gate_off() { if (stage_ != idle) stage_ = release; }
    void kill() { stage_ = idle; level_ = 0.0f; }
    Stage stage() const { return stage_; }
    bool active() const { return stage_ != idle; }
    float level() const { return level_; }

    float step(const EnvelopeShape &s) {
        switch (stage_) {
        case attack:
            level_ += s.attack * (EnvelopeShape::kAttackTarget - level_);
            if (level_ >= 1.0f) { level_ = 1.0f; stage_ = decay; }
            break;
        case decay:
            level_ += s.decay * (s.sustain - level_);
            if (level_ - s.sustain < 1e-4f) stage_ = sustain;
            break;
        case sustain:
            level_ += s.decay * (s.sustain - level_);   // follows sustain changes smoothly
            break;
        case release:
            level_ += s.release * (-0.001f - level_);   // aims just below zero so it ends in finite time
            if (level_ <= 0.0f) kill();
            break;
        case idle:
            break;
        }
        return level_;
    }

private:
    Stage stage_ = idle;
    float level_ = 0.0f;
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

}  // namespace enaga::dsp
