// Enaga: musical helpers: note names for the screen, scales (to quantise to, or to lay out on pads), and tempo
// conversions. Plain functions, for either context.
//
//     char name[5];
//     enaga::music::note_name(61, name);                            // "C#4"
//     int note = enaga::music::quantize(raw, 48, enaga::music::minor);   // to C minor
//     int pad_note = enaga::music::degree(48, enaga::music::major, pad);  // pad n plays the n-th note of C major
//     float delay_s = enaga::music::seconds(tempo, enaga::music::dotted(enaga::music::eighth));
#pragma once

#include <cstdint>

#include <enaga/dsp/math.h>

namespace enaga::music {

// ---- Note names ----

// The name of MIDI note `note` (0-127) into out: "C4", "F#2", "Bb-1". Note 60 is C4 unless middle_c_octave says
// otherwise (3 for the Yamaha convention); flats instead of sharps if asked. Up to 4 characters and the 0.
inline void note_name(int note, char out[5], bool flats = false, int middle_c_octave = 4) {
    static constexpr char letters[] = "CCDDEFFGGAAB";
    static constexpr bool black[] = {false, true, false, true, false, false, true, false, true, false, true, false};
    note = note < 0 ? 0 : note > 127 ? 127 : note;
    int pc = note % 12, octave = note / 12 + middle_c_octave - 5, i = 0;
    out[i++] = black[pc] && flats ? letters[pc + 1] : letters[pc];
    if (black[pc]) out[i++] = flats ? 'b' : '#';
    if (octave < 0) {
        out[i++] = '-';
        octave = -octave;
    }
    out[i++] = char('0' + octave % 10);
    out[i] = 0;
}

// ---- Scales: a 12-bit mask, bit i set when the scale has the note i semitones above its root ----

constexpr uint16_t chromatic = 0xFFF;
constexpr uint16_t major = 1 << 0 | 1 << 2 | 1 << 4 | 1 << 5 | 1 << 7 | 1 << 9 | 1 << 11;
constexpr uint16_t minor = 1 << 0 | 1 << 2 | 1 << 3 | 1 << 5 | 1 << 7 | 1 << 8 | 1 << 10;   // natural minor
constexpr uint16_t harmonic_minor = 1 << 0 | 1 << 2 | 1 << 3 | 1 << 5 | 1 << 7 | 1 << 8 | 1 << 11;
constexpr uint16_t melodic_minor = 1 << 0 | 1 << 2 | 1 << 3 | 1 << 5 | 1 << 7 | 1 << 9 | 1 << 11;
constexpr uint16_t dorian = 1 << 0 | 1 << 2 | 1 << 3 | 1 << 5 | 1 << 7 | 1 << 9 | 1 << 10;
constexpr uint16_t phrygian = 1 << 0 | 1 << 1 | 1 << 3 | 1 << 5 | 1 << 7 | 1 << 8 | 1 << 10;
constexpr uint16_t lydian = 1 << 0 | 1 << 2 | 1 << 4 | 1 << 6 | 1 << 7 | 1 << 9 | 1 << 11;
constexpr uint16_t mixolydian = 1 << 0 | 1 << 2 | 1 << 4 | 1 << 5 | 1 << 7 | 1 << 9 | 1 << 10;
constexpr uint16_t locrian = 1 << 0 | 1 << 1 | 1 << 3 | 1 << 5 | 1 << 6 | 1 << 8 | 1 << 10;
constexpr uint16_t major_pentatonic = 1 << 0 | 1 << 2 | 1 << 4 | 1 << 7 | 1 << 9;
constexpr uint16_t minor_pentatonic = 1 << 0 | 1 << 3 | 1 << 5 | 1 << 7 | 1 << 10;
constexpr uint16_t blues = 1 << 0 | 1 << 3 | 1 << 5 | 1 << 6 | 1 << 7 | 1 << 10;
constexpr uint16_t whole_tone = 1 << 0 | 1 << 2 | 1 << 4 | 1 << 6 | 1 << 8 | 1 << 10;

// The scales above with names, for a menu: scales[i].name, scales[i].mask.
struct Scale { const char *name; uint16_t mask; };
inline constexpr Scale scales[] = {
    {"CHROMATIC", chromatic}, {"MAJOR", major}, {"MINOR", minor}, {"HARM MINOR", harmonic_minor},
    {"MEL MINOR", melodic_minor}, {"DORIAN", dorian}, {"PHRYGIAN", phrygian}, {"LYDIAN", lydian},
    {"MIXOLYDIAN", mixolydian}, {"LOCRIAN", locrian}, {"MAJ PENTA", major_pentatonic}, {"MIN PENTA", minor_pentatonic},
    {"BLUES", blues}, {"WHOLE TONE", whole_tone},
};
inline constexpr int scale_count = sizeof scales / sizeof scales[0];

// How many notes the scale has in an octave.
constexpr int scale_size(uint16_t mask) {
    int n = 0;
    for (int i = 0; i < 12; i++) n += (mask >> i) & 1;
    return n;
}

// Whether `note` is in the scale on `root` (any note of the root's pitch class: 48 and 60 are both C).
constexpr bool in_scale(int note, int root, uint16_t mask) {
    int pc = ((note - root) % 12 + 12) % 12;
    return (mask >> pc) & 1;
}

// The note of the scale nearest `note`; between two equally near, the lower. An empty mask leaves note as it is.
constexpr int quantize(int note, int root, uint16_t mask) {
    if (!(mask & 0xFFF)) return note;
    for (int d = 0; d < 12; d++) {                        // nearest first, the lower side first
        if (in_scale(note - d, root, mask)) return note - d;
        if (in_scale(note + d, root, mask)) return note + d;
    }
    return note;
}

// The index-th note of the scale counted from `root` (a note number: 48 for C3 in Enaga's naming): 0 is root, 1
// the next note up, -1 the one below. For laying a scale across pads or steps.
constexpr int degree(int root, uint16_t mask, int index) {
    int size = scale_size(mask);
    if (!size) return root + index;
    int octave = index >= 0 ? index / size : -((-index + size - 1) / size), position = index - octave * size;
    for (int i = 0; i < 12; i++)
        if ((mask >> i) & 1) {
            if (position == 0) return root + 12 * octave + i;
            position--;
        }
    return root;
}

// ---- Pitch and tempo ----

// The frequency of a (fractional) MIDI note, A4 = 69 = 440 Hz by default.
inline float note_hz(float note, float a4 = 440.0f) { return a4 * enaga_semitones(note - 69.0f); }

// Note lengths in beats (quarter notes), to pass as `beats`: dotted(eighth) is 0.75, triplet(eighth) 1/3.
constexpr float whole = 4.0f, half = 2.0f, quarter = 1.0f, eighth = 0.5f, sixteenth = 0.25f, thirty_second = 0.125f;
constexpr float dotted(float beats) { return beats * 1.5f; }
constexpr float triplet(float beats) { return beats * (2.0f / 3.0f); }

// How long `beats` last at `bpm`: in seconds, milliseconds, or frames at `rate`; and as a frequency (an LFO or
// delay synced to the tempo: hz(120, quarter) is 2).
inline float seconds(float bpm, float beats) { return beats * 60.0f * enaga_reciprocal(bpm); }
inline float milliseconds(float bpm, float beats) { return beats * 60000.0f * enaga_reciprocal(bpm); }
inline float frames(float bpm, float beats, float rate = 48000.0f) { return seconds(bpm, beats) * rate; }
inline float hz(float bpm, float beats) { return bpm * (1.0f / 60.0f) * enaga_reciprocal(beats); }

}  // namespace enaga::music
