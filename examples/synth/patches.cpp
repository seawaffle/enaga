#include "patches.hpp"

namespace synth {
namespace {

using C = Patch::Change;
constexpr C kEnd = {param_count, 0};

constexpr C kInit[] = {kEnd};

constexpr C kBrass[] = {
    {mix_osc1, 70}, {mix_osc2, 70}, {osc2_fine, 8},
    {filter_cutoff, 50}, {filter_resonance, 15}, {filter_env, 45}, {filter_velocity, 40},
    {env2_attack, 38}, {env2_decay, 75}, {env2_sustain, 40}, {env2_release, 60},
    {amp_attack, 25}, {amp_decay, 70}, {amp_sustain, 90}, {amp_release, 60},
    {mod_source(0), src_lfo1}, {mod_destination(0), dst_pitch}, {mod_amount(0), 1},
    {reverb_mix, 20}, kEnd};

constexpr C kPad[] = {
    {mix_osc1, 60}, {mix_osc2, 60}, {osc2_fine, -11}, {osc1_fine, 4},
    {filter_cutoff, 68}, {filter_resonance, 20}, {filter_env, 20},
    {env2_attack, 100}, {env2_decay, 100}, {env2_sustain, 50}, {env2_release, 100},
    {amp_attack, 95}, {amp_decay, 100}, {amp_sustain, 90}, {amp_release, 100},
    {lfo2_shape, lfo_triangle}, {lfo2_rate, 25},
    {mod_source(0), src_lfo2}, {mod_destination(0), dst_cutoff}, {mod_amount(0), 8},
    {mod_source(1), src_random}, {mod_destination(1), dst_pan}, {mod_amount(1), 60},
    {delay_mix, 15}, {reverb_decay, 90}, {reverb_mix, 45}, kEnd};

constexpr C kBass[] = {
    {voice_mode, mode_legato}, {voice_glide, 62},
    {mix_osc1, 80}, {mix_sub, 70}, {osc2_wave, wave_pulse}, {osc2_semi, -12}, {osc2_fine, 0}, {mix_osc2, 40},
    {filter_cutoff, 38}, {filter_resonance, 30}, {filter_drive, 30}, {filter_env, 55},
    {env2_decay, 65}, {env2_sustain, 0}, {amp_decay, 80}, {amp_sustain, 80}, {amp_release, 30}, kEnd};

constexpr C kLead[] = {
    {voice_mode, mode_legato}, {voice_glide, 70},
    {osc1_wave, wave_pulse}, {osc1_width, 30}, {mix_osc2, 60}, {osc2_fine, 10},
    {filter_cutoff, 80}, {filter_resonance, 35}, {filter_env, 20}, {env2_decay, 75}, {env2_sustain, 30},
    {lfo1_shape, lfo_sine}, {lfo1_rate, 72}, {lfo1_sync, 1},
    {mod_source(0), src_lfo1}, {mod_destination(0), dst_pitch}, {mod_amount(0), 2},
    {mod_source(1), src_pressure}, {mod_destination(1), dst_cutoff}, {mod_amount(1), 30},
    {delay_time, 85}, {delay_feedback, 40}, {delay_mode, 1}, {delay_mix, 30}, {reverb_mix, 20}, kEnd};

constexpr C kPluck[] = {
    {mix_osc1, 80}, {osc2_wave, wave_pulse}, {osc2_semi, 12}, {osc2_fine, 0}, {mix_osc2, 30},
    {filter_cutoff, 40}, {filter_resonance, 40}, {filter_env, 70}, {filter_velocity, 50},
    {env2_decay, 68}, {env2_sustain, 0}, {env2_release, 60},
    {amp_decay, 82}, {amp_sustain, 0}, {amp_release, 60},
    {delay_time, 78}, {delay_feedback, 45}, {delay_mode, 1}, {delay_mix, 35}, {reverb_mix, 25}, kEnd};

constexpr C kWobble[] = {
    {voice_mode, mode_mono}, {mix_osc1, 80}, {mix_sub, 60},
    {filter_cutoff, 45}, {filter_resonance, 60}, {filter_drive, 40},
    {lfo1_shape, lfo_sine}, {lfo1_rate, 60}, {lfo1_sync, 1},
    {mod_source(0), src_lfo1}, {mod_destination(0), dst_cutoff}, {mod_amount(0), 35},
    {reverb_mix, 15}, kEnd};

constexpr C kBells[] = {
    {osc1_wave, wave_sine}, {osc2_wave, wave_sine}, {osc2_semi, 19}, {osc2_fine, 0}, {mix_osc1, 80}, {mix_osc2, 45},
    {amp_decay, 105}, {amp_sustain, 0}, {amp_release, 100}, {voice_velocity, 80},
    {mod_source(0), src_velocity}, {mod_destination(0), dst_osc2}, {mod_amount(0), 60},
    {delay_mix, 25}, {delay_mode, 1}, {reverb_decay, 85}, {reverb_mix, 50}, kEnd};

constexpr C kNoiseSweep[] = {
    {mix_osc1, 0}, {mix_noise, 80}, {filter_type, filter_bp}, {filter_cutoff, 60}, {filter_resonance, 75},
    {lfo2_shape, lfo_sample_hold}, {lfo2_rate, 80},
    {mod_source(0), src_lfo2}, {mod_destination(0), dst_cutoff}, {mod_amount(0), 40},
    {amp_attack, 40}, {amp_release, 80}, {reverb_mix, 40}, {delay_mix, 20}, kEnd};

}  // namespace

const Patch patches[] = {
    {"INIT", kInit}, {"BRASS", kBrass}, {"PAD", kPad}, {"BASS", kBass}, {"LEAD", kLead},
    {"PLUCK", kPluck}, {"WOBBLE", kWobble}, {"BELLS", kBells}, {"NOISE S+H", kNoiseSweep},
};
const int patch_count = sizeof patches / sizeof patches[0];

void load(const Patch &patch, int16_t *values) {
    for (int p = 0; p < param_count; p++) values[p] = info(p).init;
    for (const C *c = patch.changes; c->param != param_count; c++) values[c->param] = c->value;
}

}  // namespace synth
