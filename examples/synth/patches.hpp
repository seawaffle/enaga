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
