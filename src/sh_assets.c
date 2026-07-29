#include "sh_assets.h"

uint16_t sha_rd16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

int16_t sha_rds16(const uint8_t *p)
{
    return (int16_t)sha_rd16(p);
}

uint32_t sha_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int32_t sha_rds32(const uint8_t *p)
{
    return (int32_t)sha_rd32(p);
}

int32_t sha_cos30(uint16_t angle)
{
    const int16_t *table = (const int16_t *)sha_ptr(SHA_COS_Q15_OFF);
    return (int32_t)table[angle >> 6] << 15;
}

int32_t sha_sin30(uint16_t angle)
{
    /* This is the original engine convention: Sin(a) == Cos(a + 90°). */
    return sha_cos30((uint16_t)(angle + 16384u));
}

void sha_get_track(uint8_t track, SHTrackAssets *out)
{
    if ((track & 1u) == 0) {
        out->map = sha_ptr(SHA_MAP0_MAP128_OFF);
        out->tiles = sha_ptr(SHA_MAP0_TILES_OFF);
        out->sky = sha_ptr(SHA_MAP0_SKY_OFF);
        out->mountains = sha_ptr(SHA_MAP0_MOUNTAINS_OFF);
        out->path = sha_ptr(SHA_MAP0_PATH_OFF);
        out->starts = sha_ptr(SHA_MAP0_STARTS_OFF);
        out->walls = sha_ptr(SHA_MAP0_WALLS_OFF);
        out->obstacles = sha_ptr(SHA_MAP0_OBSTACLES_OFF);
        out->tile_count = SHA_MAP0_TILE_COUNT;
        out->path_count = SHA_MAP0_PATH_COUNT;
        out->start_count = SHA_MAP0_START_COUNT;
        out->wall_count = SHA_MAP0_WALL_COUNT;
        out->obstacle_count = SHA_MAP0_OBSTACLE_COUNT;
    } else {
        out->map = sha_ptr(SHA_MAP1_MAP128_OFF);
        out->tiles = sha_ptr(SHA_MAP1_TILES_OFF);
        out->sky = sha_ptr(SHA_MAP1_SKY_OFF);
        out->mountains = sha_ptr(SHA_MAP1_MOUNTAINS_OFF);
        out->path = sha_ptr(SHA_MAP1_PATH_OFF);
        out->starts = sha_ptr(SHA_MAP1_STARTS_OFF);
        out->walls = sha_ptr(SHA_MAP1_WALLS_OFF);
        out->obstacles = sha_ptr(SHA_MAP1_OBSTACLES_OFF);
        out->tile_count = SHA_MAP1_TILE_COUNT;
        out->path_count = SHA_MAP1_PATH_COUNT;
        out->start_count = SHA_MAP1_START_COUNT;
        out->wall_count = SHA_MAP1_WALL_COUNT;
        out->obstacle_count = SHA_MAP1_OBSTACLE_COUNT;
    }
}

void sha_get_sprite(uint16_t id, SHSprite *out)
{
    const uint8_t *p = sha_ptr(SHA_SPRITE_META_OFF) + (uint32_t)id * 20u;
    out->pixels = sha_ptr(sha_rd32(p));
    out->width = sha_rd16(p + 4);
    out->height = sha_rd16(p + 6);
    out->dx = sha_rds16(p + 8);
    out->dy = sha_rds16(p + 10);
    out->world_width = sha_rd32(p + 12);
    out->world_height = sha_rd32(p + 16);
}

void sha_get_path(uint8_t track, uint16_t id, SHPathPoint *out)
{
    SHTrackAssets assets;
    const uint8_t *p;
    sha_get_track(track, &assets);
    p = assets.path + (uint32_t)(id % assets.path_count) * 16u;
    out->x = sha_rd32(p);
    out->y = sha_rd32(p + 4);
    out->direction = sha_rds32(p + 8);
    out->speed = sha_rds32(p + 12);
}

void sha_get_wall(uint8_t track, uint16_t id, SHWall *out)
{
    SHTrackAssets assets;
    const uint8_t *p;
    sha_get_track(track, &assets);
    p = assets.walls + (uint32_t)(id % assets.wall_count) * 20u;
    out->x0 = sha_rd32(p);
    out->y0 = sha_rd32(p + 4);
    out->x1 = sha_rd32(p + 8);
    out->y1 = sha_rd32(p + 12);
    out->texture = sha_rd16(p + 16);
}

void sha_get_obstacle(uint8_t track, uint16_t id, SHObstacle *out)
{
    SHTrackAssets assets;
    const uint8_t *p;
    sha_get_track(track, &assets);
    p = assets.obstacles + (uint32_t)(id % assets.obstacle_count) * 12u;
    out->x = sha_rd32(p);
    out->y = sha_rd32(p + 4);
    out->angle = sha_rd16(p + 8);
    out->sprite = sha_rd16(p + 10);
}

void sha_get_model(uint8_t car_type, uint16_t id, SHModel *out)
{
    const uint8_t *p = sha_ptr(SHA_MODEL_META_OFF) +
                       ((uint32_t)(car_type & 1u) * 6u + (id % 6u)) * 8u;
    out->data = sha_ptr(sha_rd32(p));
    out->vertex_count = sha_rd16(p + 4);
    out->face_count = sha_rd16(p + 6);
}
