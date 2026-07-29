#ifndef SH_ASSETS_H
#define SH_ASSETS_H

#include <stdint.h>
#include "../assets/generated/speed_haste_assets.h"

typedef struct SHSprite {
    const uint8_t *pixels;
    uint16_t width, height;
    int16_t dx, dy;
    uint32_t world_width, world_height;
} SHSprite;

typedef struct SHPathPoint {
    uint32_t x, y;
    int32_t direction, speed;
} SHPathPoint;

typedef struct SHWall {
    uint32_t x0, y0, x1, y1;
    uint16_t texture;
} SHWall;

typedef struct SHObstacle {
    uint32_t x, y;
    uint16_t angle, sprite;
} SHObstacle;

typedef struct SHModel {
    const uint8_t *data;
    uint16_t vertex_count, face_count;
} SHModel;

uint16_t sha_rd16(const uint8_t *p);
int16_t sha_rds16(const uint8_t *p);
uint32_t sha_rd32(const uint8_t *p);
int32_t sha_rds32(const uint8_t *p);
int32_t sha_cos30(uint16_t angle);
int32_t sha_sin30(uint16_t angle);
void sha_get_sprite(uint16_t id, SHSprite *out);
void sha_get_path(uint16_t id, SHPathPoint *out);
void sha_get_wall(uint16_t id, SHWall *out);
void sha_get_obstacle(uint16_t id, SHObstacle *out);
void sha_get_model(uint16_t id, SHModel *out);

static inline const uint8_t *sha_ptr(uint32_t offset) { return SH_ASSET_BASE + offset; }

#endif
