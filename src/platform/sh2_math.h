#ifndef SPEED_HASTE_SH2_MATH_H
#define SPEED_HASTE_SH2_MATH_H
#include <stdint.h>

/* SH7604 hardware divider wrappers, adapted from D32XR's sh2_fixed.s. */
int32_t sh2_idiv(int32_t numerator, int32_t denominator);
int32_t sh2_muldiv(int32_t a, int32_t b, int32_t denominator);

typedef struct SH2FloorRowJob {
    volatile uint32_t *dst0;
    volatile uint32_t *dst1;
    const uint8_t *map;
    const uint8_t *tiles;
    const uint8_t *cached_tiles;
    const uint8_t *shade;
    uint32_t pos_x, pos_y;
    int32_t step_x, step_y;
    uint32_t cached_count;
} SH2FloorRowJob;

/* 80 floor samples -> aligned packed 4x2 framebuffer pixels. */
void sh2_floor_row(const SH2FloorRowJob *job);
/* 40 floor samples -> 8-px columns for perspective-compressed far rows. */
void sh2_floor_row_far(const SH2FloorRowJob *job);

#endif
