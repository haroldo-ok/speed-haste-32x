#ifndef SH_RENDER_WORKER_H
#define SH_RENDER_WORKER_H
#include <stdint.h>

#define SH_FLOOR_JOB ((volatile SHFloorJob *)0x26030000)
#define SH_SLAVE_IDLE 0x0000u
#define SH_SLAVE_DRAW_FLOOR 0x5346u
#define SH_SLAVE_FLOOR_DONE 0x4446u

typedef struct SHFloorJob {
    volatile uint8_t *framebuffer;
    const uint8_t *map;
    const uint8_t *tiles;
    const uint8_t *cached_tiles;
    uint32_t camera_x, camera_y;
    uint16_t angle;
    uint8_t track;
    uint8_t cached_tile_count;
    int32_t height, focus, horizon;
    volatile uint16_t profile_ticks;
    uint16_t reserved;
} SHFloorJob;

void sh_render_slave_floor(volatile SHFloorJob *job);

#endif
