// Enaga DSP: rate conversion (the C functions in <enaga/dsp.h>, "Rate conversion"). The sample format functions
// (enaga_pcm_to_float, enaga_pcm_to_int16, enaga_int16_to_float, enaga_float_to_int16) are called as they are.
//
//     enaga::dsp::Resampler left, right;                    // in init(): the table comes from the app heap
//     left.init(wav.rate, ENAGA_SAMPLE_RATE);
//     right.init(left);                                     // the same rates: shares left's table
//     int used, made = left.process(in, n, out, room, used);
#pragma once

#include <enaga/dsp.h>
#include <enaga/dsp/math.hpp>

namespace enaga::dsp {

class Resampler {
public:
    // A conversion from in_rate to out_rate (Hz), from the start of a sound; makes the table (16.6 KB).
    void init(uint32_t in_rate, uint32_t out_rate) {
        float *table = memory_.allocate(enaga_resample_table_size());
        enaga_resample_table(table, in_rate, out_rate);
        enaga_resampler_init(&state_, table, in_rate, out_rate);
        in_rate_ = in_rate;
        out_rate_ = out_rate;
    }
    // Another channel at the same rates, sharing other's table (other must outlive this one).
    void init(const Resampler &other) {
        enaga_resampler_init(&state_, other.state_.table, other.in_rate_, other.out_rate_);
        in_rate_ = other.in_rate_;
        out_rate_ = other.out_rate_;
    }
    // Back to the start of a sound, at the same rates.
    void reset() { enaga_resampler_init(&state_, state_.table, in_rate_, out_rate_); }
    // enaga_resample: returns the outputs written; used is set to the inputs taken.
    int process(const float *in, int n_in, float *out, int max_out, int &used) {
        return enaga_resample(&state_, in, n_in, out, max_out, &used);
    }

    enaga_resampler state_{};

private:
    Memory memory_;
    uint32_t in_rate_ = 0, out_rate_ = 0;
};

}  // namespace enaga::dsp
