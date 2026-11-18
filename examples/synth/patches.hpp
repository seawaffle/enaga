// Factory patches: each is the INIT values (the `init` column of the parameter table) with a few changes.
#pragma once

#include "params.hpp"

namespace synth {

struct Patch {
    const char *name;
    struct Change { uint8_t param; int16_t value; };
    const Change *changes;      // ends with param == param_count
};

extern const Patch patches[];
extern const int patch_count;

// Writes the patch's values into values[0 .. param_count).
void load(const Patch &patch, int16_t *values);

}  // namespace synth

namespace synth {

// User patches, saved on the card in the app's data folder: user_slots of them, "USER 01.SYN" to "USER 16.SYN",
// and LAST.SYN, the sound when the app stopped, restored when it starts.
//
// A file: "ESYN", version (1), sets (1 for a patch; 2 for LAST: the values, then the patch they were edited from),
// the parameter count, the patch-list entry it came from (-1 in a patch file), a name of up to 15 characters
// (zero-padded to 16), then sets x count int16 values; all little-endian. Parameters keep their numbers (new ones
// go at the end), so a file from an older synth loads with INIT values for what it lacks.
inline constexpr int user_slots = 16;
inline constexpr int name_size = 16;
inline constexpr uint32_t file_max = 26 + 2 * 2 * param_count;

struct Saved {
    char name[name_size];
    int16_t entry;              // patch-list entry (LAST only)
    int sets;
    const int16_t *values;      // encode: the values
    const int16_t *base;        // encode, sets == 2: the patch they were edited from
};

// Writes the file image into out (file_max bytes); returns its size.
uint32_t encode(const Saved &saved, uint8_t *out);

// Reads a file image: name, entry, and the values (and base, when the file has two sets and base is given), each
// clamped to its range and INIT where the file has none. False if it is not a patch file.
bool decode(const uint8_t *data, int32_t bytes, char *name, int16_t *entry, int16_t *values, int16_t *base);

}  // namespace synth
