#include "sh_assets.h"
#include "platform/sh2_math.h"

/*
 * D32XR keeps hot texture/lump data in aligned SDRAM caches instead of
 * repeatedly contending for the cartridge bus. The first 28 atlas entries are
 * frequency-sorted by the importer and cover the large majority of map cells.
 */
#define CACHE_THROUGH(ptr) ((uint8_t *)((uintptr_t)(ptr) | 0x20000000u))
#define MAX_SKY_BYTES 22400u
#define MAX_MOUNTAIN_BYTES 15680u

static uint8_t track_map_cache[128u * 128u] __attribute__((aligned(16)));
static uint8_t tile_cache[SHA_TILE_CACHE_COUNT * 4096u] __attribute__((aligned(16)));
static uint8_t color_map_cache[8192u] __attribute__((aligned(16)));
static uint8_t sky_cache[MAX_SKY_BYTES] __attribute__((aligned(16)));
static uint8_t mountain_cache[MAX_MOUNTAIN_BYTES] __attribute__((aligned(16)));
static uint8_t active_track = 0xFFu;
static uint8_t color_map_ready;

static void copy_aligned(void *destination, const void *source, uint32_t bytes)
{
    volatile uint32_t *dst = (volatile uint32_t *)CACHE_THROUGH(destination);
    const uint32_t *src = (const uint32_t *)source;
    uint32_t words = bytes >> 2;

    /* Four-word unrolling mirrors D32XR's preference for short, aligned hot loops. */
    while (words >= 4) {
        dst[0] = src[0]; dst[1] = src[1];
        dst[2] = src[2]; dst[3] = src[3];
        dst += 4; src += 4; words -= 4;
    }
    while (words--) *dst++ = *src++;
}

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

uint16_t sha_vector_angle(int32_t dx, int32_t dy)
{
    uint32_t ax = (uint32_t)(dx < 0 ? -dx : dx);
    uint32_t ay = (uint32_t)(dy < 0 ? -dy : dy);
    uint16_t base;
    if ((ax | ay) == 0) return 0;
    if (ax >= ay)
        base = (uint16_t)sh2_muldiv((int32_t)ay, 8192, (int32_t)ax);
    else
        base = (uint16_t)(16384 - sh2_muldiv((int32_t)ax, 8192, (int32_t)ay));
    if (dx >= 0 && dy >= 0) return base;
    if (dx < 0 && dy >= 0) return (uint16_t)(32768u - base);
    if (dx < 0) return (uint16_t)(32768u + base);
    return (uint16_t)(0u - base);
}

static void sha_get_track_raw(uint8_t track, SHTrackAssets *out)
{
    out->cached_tiles = 0;
    out->cached_tile_count = 0;
    if ((track & 1u) == 0) {
        out->map = sha_ptr(SHA_MAP0_MAP128_OFF);
        out->tiles = sha_ptr(SHA_MAP0_TILES_OFF);
        out->sky = sha_ptr(SHA_MAP0_SKY_OFF);
        out->mountains = sha_ptr(SHA_MAP0_MOUNTAINS_OFF);
        out->path = sha_ptr(SHA_MAP0_PATH_OFF);
        out->starts = sha_ptr(SHA_MAP0_STARTS_OFF);
        out->cameras = sha_ptr(SHA_MAP0_CAMERAS_OFF);
        out->walls = sha_ptr(SHA_MAP0_WALLS_OFF);
        out->obstacles = sha_ptr(SHA_MAP0_OBSTACLES_OFF);
        out->tile_count = SHA_MAP0_TILE_COUNT;
        out->path_count = SHA_MAP0_PATH_COUNT;
        out->start_count = SHA_MAP0_START_COUNT;
        out->camera_count = SHA_MAP0_CAMERA_COUNT;
        out->wall_count = SHA_MAP0_WALL_COUNT;
        out->obstacle_count = SHA_MAP0_OBSTACLE_COUNT;
    } else {
        out->map = sha_ptr(SHA_MAP1_MAP128_OFF);
        out->tiles = sha_ptr(SHA_MAP1_TILES_OFF);
        out->sky = sha_ptr(SHA_MAP1_SKY_OFF);
        out->mountains = sha_ptr(SHA_MAP1_MOUNTAINS_OFF);
        out->path = sha_ptr(SHA_MAP1_PATH_OFF);
        out->starts = sha_ptr(SHA_MAP1_STARTS_OFF);
        out->cameras = sha_ptr(SHA_MAP1_CAMERAS_OFF);
        out->walls = sha_ptr(SHA_MAP1_WALLS_OFF);
        out->obstacles = sha_ptr(SHA_MAP1_OBSTACLES_OFF);
        out->tile_count = SHA_MAP1_TILE_COUNT;
        out->path_count = SHA_MAP1_PATH_COUNT;
        out->start_count = SHA_MAP1_START_COUNT;
        out->camera_count = SHA_MAP1_CAMERA_COUNT;
        out->wall_count = SHA_MAP1_WALL_COUNT;
        out->obstacle_count = SHA_MAP1_OBSTACLE_COUNT;
    }
}

void sha_prepare_track(uint8_t track)
{
    SHTrackAssets raw;
    uint32_t cached_tiles;
    uint32_t sky_bytes, mountain_bytes;
    track &= 1u;
    if (active_track == track) return;

    sha_get_track_raw(track, &raw);
    cached_tiles = raw.tile_count < SHA_TILE_CACHE_COUNT ?
                   raw.tile_count : SHA_TILE_CACHE_COUNT;
    sky_bytes = track ? SHA_MAP1_SKY_SIZE : SHA_MAP0_SKY_SIZE;
    mountain_bytes = track ? SHA_MAP1_MOUNTAINS_SIZE : SHA_MAP0_MOUNTAINS_SIZE;

    copy_aligned(track_map_cache, raw.map, sizeof(track_map_cache));
    copy_aligned(tile_cache, raw.tiles, cached_tiles * 4096u);
    copy_aligned(sky_cache, raw.sky, sky_bytes);
    copy_aligned(mountain_cache, raw.mountains, mountain_bytes);
    if (!color_map_ready) {
        copy_aligned(color_map_cache, sha_ptr(SHA_COLOR_MAP_OFF), sizeof(color_map_cache));
        color_map_ready = 1;
    }
    active_track = track;
}

const uint8_t *sha_color_map(void)
{
    if (!color_map_ready) {
        copy_aligned(color_map_cache, sha_ptr(SHA_COLOR_MAP_OFF), sizeof(color_map_cache));
        color_map_ready = 1;
    }
    return CACHE_THROUGH(color_map_cache);
}

void sha_get_track(uint8_t track, SHTrackAssets *out)
{
    track &= 1u;
    sha_get_track_raw(track, out);
    if (active_track == track) {
        out->map = CACHE_THROUGH(track_map_cache);
        out->cached_tiles = CACHE_THROUGH(tile_cache);
        out->cached_tile_count = out->tile_count < SHA_TILE_CACHE_COUNT ?
                                 out->tile_count : SHA_TILE_CACHE_COUNT;
        out->sky = CACHE_THROUGH(sky_cache);
        out->mountains = CACHE_THROUGH(mountain_cache);
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

void sha_get_camera(uint8_t track, uint16_t id, SHStaticCamera *out)
{
    SHTrackAssets assets;
    const uint8_t *p;
    sha_get_track(track, &assets);
    p = assets.cameras + (uint32_t)(id % assets.camera_count) * 12u;
    out->x = sha_rd32(p);
    out->y = sha_rd32(p + 4);
    out->height = sha_rd32(p + 8);
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
    out->flags = sha_rd16(p + 18);
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
