// Enaga DSP: dynamics (a compressor and a look-ahead limiter) and a bitcrusher. Provisional until 1.0.
//
//     enaga::dsp::Limiter limiter_;                         // in init(): its delay lines come from the app heap
//     limiter_.init(0.002f, ENAGA_SAMPLE_RATE);             // 2 ms look-ahead
//     limiter_.set(-0.3f, 0.1f);                            // ceiling -0.3 dB, 100 ms release
//     limiter_.process(left, right, frames);                // in audio(), last: nothing leaves above -0.3 dB
//
//     enaga::dsp::Compressor duck_;                          // ducking: the pad's level, keyed by the kick
//     duck_.process(pad_l, pad_r, frames, kick_l, kick_r);
#pragma once

#include <enaga/dsp.h>
#include <enaga/dsp/math.hpp>

namespace enaga::dsp {

// A compressor: feed-forward, the channels linked (both get the same gain), peak detection, a soft knee, and the
// gain reduction moving in decibels with the attack and release. It can be keyed by another signal (a side
// chain) to duck one sound under another. enaga_compressor_*: block functions.
class Compressor {
public:
    explicit Compressor(float sample_rate = 48000.0f) { enaga_compressor_init(&state_, sample_rate); }
    void init(float sample_rate) { enaga_compressor_init(&state_, sample_rate); }
    // threshold_db (0 and below); ratio 1 or more (4 firm, 20 or more limiting); attack_s and release_s time
    // constants (5 ms and 100 ms are a start); knee_db the soft knee's width (0 hard); makeup_db gain after.
    void set(float threshold_db, float ratio, float attack_s, float release_s, float knee_db = 6.0f,
             float makeup_db = 0.0f) {
        enaga_compressor_set(&state_, threshold_db, ratio, attack_s, release_s, knee_db, makeup_db);
    }
    // In place; right may be nullptr (mono). Keyed by its own level, or by key_l / key_r (key_r nullptr: mono).
    void process(float *left, float *right, int n, const float *key_l = nullptr, const float *key_r = nullptr) {
        enaga_compressor_process(&state_, left, right, n, key_l, key_r);
    }
    // The gain reduction now, in dB (0 or more): for a meter.
    float reduction_db() const { return state_.reduction; }
    void reset() { state_.reduction = 0.0f; }

    enaga_compressor state_{};
};

// A limiter that looks ahead: each peak's gain is reached smoothly over the look-ahead before the peak gets
// there, so no sample leaves above the ceiling and nothing clicks. The signal comes out latency() samples late
// (the look-ahead, less one); put it last, on the whole mix. enaga_limiter_*: block functions.
class Limiter {
public:
    // lookahead_s: 1-5 ms is usual (longer is smoother and later); 16 to 4096 samples.
    void init(float lookahead_s, float sample_rate) {
        enaga_limiter_init(&state_, memory_.allocate(enaga_limiter_memory(lookahead_s, sample_rate)), lookahead_s,
                           sample_rate);
    }
    // ceiling_db: nothing leaves above it; release_s: the recovery's time constant.
    void set(float ceiling_db, float release_s) { enaga_limiter_set(&state_, ceiling_db, release_s); }
    // In place; right may be nullptr (mono).
    void process(float *left, float *right, int n) { enaga_limiter_process(&state_, left, right, n); }
    uint32_t latency() const { return enaga_limiter_latency(&state_); }
    // The most gain reduction in the last process call, in dB (0 or more): for a meter.
    float reduction_db() const { return -enaga_gain_to_db(state_.lowest); }

    enaga_limiter state_{};

private:
    Memory memory_;
};

// A bitcrusher: fewer bits (2^bits levels across -1 to 1, fractions of a bit allowed) and a lower sample rate
// (each sample taken is held until the next), with no filtering, so it aliases: that is the sound.
// enaga_crusher_*: block functions.
class Bitcrusher {
public:
    Bitcrusher() { enaga_crusher_init(&state_); }
    // bits 1-24; rate_hz up to the sample rate.
    void set(float bits, float rate_hz, float sample_rate = 48000.0f) {
        enaga_crusher_set(&state_, bits, rate_hz, sample_rate);
    }
    // In place; right may be nullptr (mono).
    void process(float *left, float *right, int n) { enaga_crusher_process(&state_, left, right, n); }

    enaga_crusher state_{};
};

}  // namespace enaga::dsp
