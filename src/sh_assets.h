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
    uint16_t texture, flags;
} SHWall;

enum { SH_WALL_COLLIDABLE = 0x0001 };

typedef struct SHSector {
    uint16_t first_side;
    uint8_t side_count, flags;
    uint16_t min_x, min_y, max_x, max_y;
} SHSector;

typedef struct SHSectorSide {
    uint16_t v0, v1;
    int16_t other;
    uint16_t wall;
} SHSectorSide;

typedef struct SHSectorVertex { uint16_t x, y; } SHSectorVertex;
typedef struct SHSectorObjectRange { uint16_t first, count; } SHSectorObjectRange;

typedef struct SHStaticCamera {
    uint32_t x, y, height;
} SHStaticCamera;

typedef struct SHObstacle {
    uint32_t x, y;
    uint16_t angle, sprite;
} SHObstacle;

typedef struct SHModel {
    const uint8_t *data;
    uint16_t vertex_count, face_count;
} SHModel;

typedef struct SHTrackAssets {
    const uint8_t *map;
    const uint8_t *tiles;
    const uint8_t *cached_tiles;
    const uint8_t *sky;
    const uint8_t *mountains;
    const uint8_t *path;
    const uint8_t *starts;
    const uint8_t *cameras;
    const uint8_t *walls;
    const uint8_t *sector_vertices;
    const uint8_t *sectors;
    const uint8_t *sector_sides;
    const uint8_t *sector_object_meta;
    const uint8_t *sector_object_indices;
    const uint8_t *default_object_bin_meta;
    const uint8_t *default_object_bin_indices;
    const uint8_t *obstacles;
    uint16_t tile_count, cached_tile_count;
    uint16_t path_count, start_count;
    uint16_t camera_count, wall_count, obstacle_count;
    uint16_t sector_vertex_count, sector_count, sector_side_count;
    uint16_t sector_object_bucket_count;
} SHTrackAssets;

uint16_t sha_rd16(const uint8_t *p);
int16_t sha_rds16(const uint8_t *p);
uint32_t sha_rd32(const uint8_t *p);
int32_t sha_rds32(const uint8_t *p);
int32_t sha_cos30(uint16_t angle);
int32_t sha_sin30(uint16_t angle);
uint16_t sha_vector_angle(int32_t dx, int32_t dy);
void sha_prepare_track(uint8_t track);
const uint8_t *sha_color_map(void);
const uint8_t *sha_packed_shade_row(int level);
void sha_get_track(uint8_t track, SHTrackAssets *out);
void sha_get_sprite(uint16_t id, SHSprite *out);
void sha_get_path(uint8_t track, uint16_t id, SHPathPoint *out);
void sha_get_camera(uint8_t track, uint16_t id, SHStaticCamera *out);
void sha_get_wall(uint8_t track, uint16_t id, SHWall *out);
void sha_get_sector(const SHTrackAssets *assets, uint16_t id, SHSector *out);
void sha_get_sector_side(const SHTrackAssets *assets, uint16_t id, SHSectorSide *out);
void sha_get_sector_vertex(const SHTrackAssets *assets, uint16_t id, SHSectorVertex *out);
void sha_get_sector_object_range(const SHTrackAssets *assets, uint16_t sector,
                                 SHSectorObjectRange *out);
uint16_t sha_get_sector_object_index(const SHTrackAssets *assets, uint16_t id);
void sha_get_default_object_bin_range(const SHTrackAssets *assets, uint16_t bin,
                                      SHSectorObjectRange *out);
uint16_t sha_get_default_object_bin_index(const SHTrackAssets *assets, uint16_t id);
int sha_sector_contains(const SHTrackAssets *assets, int16_t sector,
                        uint32_t world_x, uint32_t world_y);
int16_t sha_find_sector(const SHTrackAssets *assets, int16_t hint,
                        uint32_t world_x, uint32_t world_y);
void sha_get_obstacle(uint8_t track, uint16_t id, SHObstacle *out);
void sha_get_model(uint8_t car_type, uint16_t id, SHModel *out);

static inline const uint8_t *sha_ptr(uint32_t offset) { return SH_ASSET_BASE + offset; }

#endif
