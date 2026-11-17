// Synth parameters: one table drives the engine, the pages, and the patches.
//
// Every parameter is a small integer in [min, max], so the encoders step it, patches store it, and the main
// context can change it with a single word write that the audio context picks up at its next block.
#pragma once

#include <cstdint>

namespace synth {

enum class Unit : uint8_t {
    number,     // plain
    signed_,    // with a sign
    choice,     // one of `choices`
    time,       // 0-127 -> 0.5 ms to 19 s (seconds())
    cutoff,     // 0-127 -> 14 Hz to 20 kHz
    rate,       // 0-127 -> 0.05 to 24 Hz
    decay,      // 0-127 -> 0.2 to 16 s
    delay_time  // 0-127 -> 10 ms to 1.5 s
};

enum Param : uint8_t {
    // OSC page
    osc1_wave, osc1_width, osc1_semi, osc1_fine,
    osc2_wave, osc2_width, osc2_semi, osc2_fine,
    mix_osc1, mix_osc2, mix_sub, mix_noise,
    // FILTER
    filter_type, filter_cutoff, filter_resonance, filter_drive,
    filter_env, filter_key, filter_velocity,
    // ENV
    amp_attack, amp_decay, amp_sustain, amp_release,
    env2_attack, env2_decay, env2_sustain, env2_release,
    env3_attack, env3_decay, env3_sustain, env3_release,
    // LFO
    lfo1_shape, lfo1_rate, lfo1_sync,
    lfo2_shape, lfo2_rate, lfo2_sync,
    // MOD: eight slots of source, destination, amount
    mod_first,
    mod_last = mod_first + 8 * 3 - 1,
    // FX
    delay_time, delay_feedback, delay_tone, delay_mix, delay_mode,
    reverb_decay, reverb_damping, reverb_mix,
    // VOICE
    voice_mode, voice_glide, voice_velocity, voice_volume, voice_layout,
    param_count
};

inline constexpr int mod_slots = 8;
inline constexpr Param mod_source(int slot) { return Param(mod_first + 3 * slot); }
inline constexpr Param mod_destination(int slot) { return Param(mod_first + 3 * slot + 1); }
inline constexpr Param mod_amount(int slot) { return Param(mod_first + 3 * slot + 2); }

enum WaveChoice : uint8_t { wave_saw, wave_pulse, wave_triangle, wave_sine };
enum FilterType : uint8_t { filter_lp12, filter_lp24, filter_bp, filter_hp };
enum LfoShapeChoice : uint8_t { lfo_sine, lfo_triangle, lfo_saw, lfo_square, lfo_sample_hold };
enum VoiceMode : uint8_t { mode_poly, mode_mono, mode_legato };
enum Layout : uint8_t { layout_piano, layout_chromatic };
enum Source : uint8_t { src_off, src_env2, src_env3, src_lfo1, src_lfo2, src_velocity, src_pressure, src_key, src_random,
                        source_count };
enum Destination : uint8_t { dst_pitch, dst_pitch1, dst_pitch2, dst_width, dst_osc1, dst_osc2, dst_noise, dst_cutoff,
                             dst_resonance, dst_amp, dst_pan, dst_lfo1_rate, dst_lfo2_rate, destination_count };

struct ParamInfo {
    const char *name;           // up to 6 characters
    int16_t min, max, init;     // init: the INIT patch
    Unit unit;
    const char *const *choices;
};

const ParamInfo &info(int param);

// Physical values for the curved units.
float seconds(int value);       // Unit::time
float cutoff_hz(int value);     // Unit::cutoff
float rate_hz(int value);       // Unit::rate
float decay_s(int value);       // Unit::decay
float delay_s(int value);       // Unit::delay_time

// Writes the value as shown on screen ("12.5S", "880HZ", "SAW", "-12") into text (at least 8 bytes).
void format(int param, int value, char *text);

}  // namespace synth
