#include "params.hpp"

#include <enaga/dsp/math.hpp>

namespace synth {
namespace {

const char *const kWaves[] = {"SAW", "PULSE", "TRI", "SINE"};
const char *const kFilters[] = {"LP12", "LP24", "BP", "HP"};
const char *const kShapes[] = {"SINE", "TRI", "SAW", "SQUARE", "S+H"};
const char *const kSync[] = {"FREE", "KEY"};
const char *const kModes[] = {"POLY", "MONO", "LEGATO"};
const char *const kDelayModes[] = {"STEREO", "PING"};
const char *const kLayouts[] = {"PIANO", "CHROMA"};
const char *const kSources[] = {"OFF", "ENV2", "ENV3", "LFO1", "LFO2", "VELO", "PRESS", "KEY", "RANDOM"};
const char *const kDestinations[] = {"PITCH", "PITCH1", "PITCH2", "WIDTH", "OSC1", "OSC2", "NOISE", "CUTOFF",
                                     "RESO", "AMP", "PAN", "L1RATE", "L2RATE"};

constexpr ParamInfo kTable[] = {
    {"WAVE", 0, 3, wave_saw, Unit::choice, kWaves},
    {"WIDTH", 5, 95, 50, Unit::number, nullptr},
    {"SEMI", -24, 24, 0, Unit::signed_, nullptr},
    {"FINE", -50, 50, 0, Unit::signed_, nullptr},
    {"WAVE", 0, 3, wave_saw, Unit::choice, kWaves},
    {"WIDTH", 5, 95, 50, Unit::number, nullptr},
    {"SEMI", -24, 24, 0, Unit::signed_, nullptr},
    {"FINE", -50, 50, 7, Unit::signed_, nullptr},
    {"OSC1", 0, 100, 80, Unit::number, nullptr},
    {"OSC2", 0, 100, 0, Unit::number, nullptr},
    {"SUB", 0, 100, 0, Unit::number, nullptr},
    {"NOISE", 0, 100, 0, Unit::number, nullptr},

    {"TYPE", 0, 3, filter_lp24, Unit::choice, kFilters},
    {"CUTOFF", 0, 127, 127, Unit::cutoff, nullptr},
    {"RESO", 0, 100, 0, Unit::number, nullptr},
    {"DRIVE", 0, 100, 0, Unit::number, nullptr},
    {"ENV2", -100, 100, 0, Unit::signed_, nullptr},
    {"KEY", 0, 100, 0, Unit::number, nullptr},
    {"VELO", 0, 100, 0, Unit::number, nullptr},

    {"ATTACK", 0, 127, 0, Unit::time, nullptr},
    {"DECAY", 0, 127, 70, Unit::time, nullptr},
    {"SUSTN", 0, 100, 100, Unit::number, nullptr},
    {"RELEAS", 0, 127, 50, Unit::time, nullptr},
    {"ATTACK", 0, 127, 0, Unit::time, nullptr},
    {"DECAY", 0, 127, 70, Unit::time, nullptr},
    {"SUSTN", 0, 100, 0, Unit::number, nullptr},
    {"RELEAS", 0, 127, 50, Unit::time, nullptr},
    {"ATTACK", 0, 127, 0, Unit::time, nullptr},
    {"DECAY", 0, 127, 70, Unit::time, nullptr},
    {"SUSTN", 0, 100, 0, Unit::number, nullptr},
    {"RELEAS", 0, 127, 50, Unit::time, nullptr},

    {"SHAPE", 0, 4, lfo_triangle, Unit::choice, kShapes},
    {"RATE", 0, 127, 64, Unit::rate, nullptr},
    {"SYNC", 0, 1, 0, Unit::choice, kSync},
    {"SHAPE", 0, 4, lfo_sine, Unit::choice, kShapes},
    {"RATE", 0, 127, 40, Unit::rate, nullptr},
    {"SYNC", 0, 1, 0, Unit::choice, kSync},
};

constexpr ParamInfo kMod[3] = {
    {"SOURCE", 0, source_count - 1, src_off, Unit::choice, kSources},
    {"DEST", 0, destination_count - 1, dst_pitch, Unit::choice, kDestinations},
    {"AMOUNT", -100, 100, 0, Unit::signed_, nullptr},
};

constexpr ParamInfo kAfterMod[] = {
    {"TIME", 0, 127, 80, Unit::delay_time, nullptr},
    {"FDBK", 0, 95, 35, Unit::number, nullptr},
    {"TONE", 0, 100, 60, Unit::number, nullptr},
    {"MIX", 0, 100, 0, Unit::number, nullptr},
    {"MODE", 0, 1, 0, Unit::choice, kDelayModes},
    {"DECAY", 0, 127, 60, Unit::decay, nullptr},
    {"DAMP", 0, 100, 40, Unit::number, nullptr},
    {"MIX", 0, 100, 0, Unit::number, nullptr},

    {"MODE", 0, 2, mode_poly, Unit::choice, kModes},
    {"GLIDE", 0, 127, 0, Unit::time, nullptr},
    {"VELO", 0, 100, 50, Unit::number, nullptr},
    {"VOLUME", 0, 100, 70, Unit::number, nullptr},
    {"PADS", 0, 1, layout_piano, Unit::choice, kLayouts},
};

static_assert(sizeof kTable / sizeof kTable[0] == mod_first, "parameter table out of step with Param");
static_assert(sizeof kAfterMod / sizeof kAfterMod[0] == param_count - mod_last - 1, "parameter table out of step");

void append(char *&out, const char *text) { while (*text) *out++ = *text++; }
void append_int(char *&out, int value) {
    if (value < 0) { *out++ = '-'; value = -value; }
    char digits[12];
    int n = 0;
    do { digits[n++] = char('0' + value % 10); value /= 10; } while (value);
    while (n) *out++ = digits[--n];
}
// value with one decimal when under 10: "4.5", "12"
void append_short(char *&out, float value) {
    if (value < 9.95f) {
        int tenths = int(value * 10.0f + 0.5f);
        append_int(out, tenths / 10);
        *out++ = '.';
        *out++ = char('0' + tenths % 10);
    } else {
        append_int(out, int(value + 0.5f));
    }
}

}  // namespace

const ParamInfo &info(int param) {
    if (param < mod_first) return kTable[param];
    if (param <= mod_last) return kMod[(param - mod_first) % 3];
    return kAfterMod[param - mod_last - 1];
}

float seconds(int value) { return 0.0005f * enaga::dsp::exp2(float(value) * 0.12f); }
float cutoff_hz(int value) { return 440.0f * enaga::dsp::semitones(float(value + 8 - 69)); }
float rate_hz(int value) { return 0.05f * enaga::dsp::exp2(float(value) * 0.07f); }
float decay_s(int value) { return 0.2f * enaga::dsp::exp2(float(value) * 0.05f); }
float delay_s(int value) { return 0.01f * enaga::dsp::exp2(float(value) * 0.0569f); }

void format(int param, int value, char *text) {
    const ParamInfo &p = info(param);
    char *out = text;
    switch (p.unit) {
    case Unit::number: append_int(out, value); break;
    case Unit::signed_:
        if (value > 0) *out++ = '+';
        append_int(out, value);
        break;
    case Unit::choice: append(out, p.choices[value]); break;
    case Unit::time:
    case Unit::decay:
    case Unit::delay_time: {
        float s = p.unit == Unit::time ? seconds(value) : p.unit == Unit::decay ? decay_s(value) : delay_s(value);
        if (param == voice_glide && value == 0) { append(out, "OFF"); break; }
        if (s < 1.0f) { append_short(out, s * 1000.0f); append(out, "MS"); }
        else { append_short(out, s); *out++ = 'S'; }
        break;
    }
    case Unit::cutoff:
    case Unit::rate: {
        float hz = p.unit == Unit::cutoff ? cutoff_hz(value) : rate_hz(value);
        if (hz >= 1000.0f) { append_short(out, hz * 0.001f); append(out, "K"); }
        else append_short(out, hz);
        append(out, "HZ");
        break;
    }
    }
    *out = 0;
}

}  // namespace synth
