/* <math.h> from C (include/libm/math.h). */
#include <math.h>

float math_from_c(float x) { return isnan(x) ? NAN : sinf(x) + sqrtf(fabsf(x)); }
