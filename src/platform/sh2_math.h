#ifndef SPEED_HASTE_SH2_MATH_H
#define SPEED_HASTE_SH2_MATH_H
#include <stdint.h>

/* SH7604 hardware divider wrappers, adapted from D32XR's sh2_fixed.s. */
int32_t sh2_idiv(int32_t numerator, int32_t denominator);
int32_t sh2_muldiv(int32_t a, int32_t b, int32_t denominator);

#endif
