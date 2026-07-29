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

void sha_get_path(uint16_t id, SHPathPoint *out)
{
    const uint8_t *p = sha_ptr(SHA_PATH_OFF) + (uint32_t)id * 16u;
    out->x = sha_rd32(p);
    out->y = sha_rd32(p + 4);
    out->direction = sha_rds32(p + 8);
    out->speed = sha_rds32(p + 12);
}

void sha_get_wall(uint16_t id, SHWall *out)
{
    const uint8_t *p = sha_ptr(SHA_WALLS_OFF) + (uint32_t)id * 20u;
    out->x0 = sha_rd32(p);
    out->y0 = sha_rd32(p + 4);
    out->x1 = sha_rd32(p + 8);
    out->y1 = sha_rd32(p + 12);
    out->texture = sha_rd16(p + 16);
}

void sha_get_obstacle(uint16_t id, SHObstacle *out)
{
    const uint8_t *p = sha_ptr(SHA_OBSTACLES_OFF) + (uint32_t)id * 12u;
    out->x = sha_rd32(p);
    out->y = sha_rd32(p + 4);
    out->angle = sha_rd16(p + 8);
    out->sprite = sha_rd16(p + 10);
}

void sha_get_model(uint16_t id, SHModel *out)
{
    const uint8_t *p = sha_ptr(SHA_MODEL_META_OFF) + (uint32_t)id * 8u;
    out->data = sha_ptr(sha_rd32(p));
    out->vertex_count = sha_rd16(p + 4);
    out->face_count = sha_rd16(p + 6);
}
