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
#define MAX_SECTOR_VERTICES ((SHA_MAP0_SECTOR_VERTEX_COUNT > SHA_MAP1_SECTOR_VERTEX_COUNT) ? SHA_MAP0_SECTOR_VERTEX_COUNT : SHA_MAP1_SECTOR_VERTEX_COUNT)
#define MAX_SECTORS ((SHA_MAP0_SECTOR_COUNT > SHA_MAP1_SECTOR_COUNT) ? SHA_MAP0_SECTOR_COUNT : SHA_MAP1_SECTOR_COUNT)
#define MAX_SECTOR_SIDES ((SHA_MAP0_SECTOR_SIDE_COUNT > SHA_MAP1_SECTOR_SIDE_COUNT) ? SHA_MAP0_SECTOR_SIDE_COUNT : SHA_MAP1_SECTOR_SIDE_COUNT)
#define MAX_SECTOR_OBJECT_BUCKETS ((SHA_MAP0_SECTOR_OBJECT_BUCKET_COUNT > SHA_MAP1_SECTOR_OBJECT_BUCKET_COUNT) ? SHA_MAP0_SECTOR_OBJECT_BUCKET_COUNT : SHA_MAP1_SECTOR_OBJECT_BUCKET_COUNT)
#define MAX_OBSTACLES ((SHA_MAP0_OBSTACLE_COUNT > SHA_MAP1_OBSTACLE_COUNT) ? SHA_MAP0_OBSTACLE_COUNT : SHA_MAP1_OBSTACLE_COUNT)

static uint8_t track_map_cache[128u * 128u] __attribute__((aligned(16)));
static uint8_t tile_cache[SHA_TILE_CACHE_COUNT * 4096u] __attribute__((aligned(16)));
static uint8_t color_map_cache[8192u] __attribute__((aligned(16)));
static uint8_t sky_cache[MAX_SKY_BYTES] __attribute__((aligned(16)));
static uint8_t sector_vertex_cache[MAX_SECTOR_VERTICES * 4u]
    __attribute__((aligned(16)));
static uint8_t sector_cache[MAX_SECTORS * 12u]
    __attribute__((aligned(16)));
static uint8_t sector_side_cache[MAX_SECTOR_SIDES * 8u]
    __attribute__((aligned(16)));
static uint8_t sector_object_meta_cache[MAX_SECTOR_OBJECT_BUCKETS * 4u]
    __attribute__((aligned(16)));
static uint8_t sector_object_index_cache[(MAX_OBSTACLES * 2u + 3u) & ~3u]
    __attribute__((aligned(16)));
static uint8_t default_object_bin_meta_cache[256u * 4u]
    __attribute__((aligned(16)));
static uint8_t default_object_bin_index_cache[(MAX_OBSTACLES * 2u + 3u) & ~3u]
    __attribute__((aligned(16)));
/* Wall/obstacle records and the sprite meta table are read from cartridge ROM
 * on almost every frame in the draw loops. Cache them in SDRAM so the render
 * critical path stops contending for the shared cartridge bus. */
static uint8_t wall_cache[SHA_MAP1_WALL_COUNT * 20u] __attribute__((aligned(16)));
static uint8_t obstacle_cache[MAX_OBSTACLES * 12u] __attribute__((aligned(16)));
static uint8_t sprite_meta_cache[SHA_SPRITE_COUNT * 20u] __attribute__((aligned(16)));
static uint8_t active_track = 0xFFu;
static uint8_t color_map_ready;

/* Precomputed packed shade rows. For each of the 16 shade levels the color-map
 * row is expanded so a byte can be turned into a 32-bit 4x-packed pixel with a
 * single indexed load, removing the per-sample shade lookup + replicate. */
static uint32_t packed_shade_cache[16u * 256u] __attribute__((aligned(16)));
static uint8_t packed_shade_ready;

/* Obstacle, particle and HUD sprites are fetched from cartridge ROM on nearly
 * every frame. Cache them in aligned SDRAM cache-through so the fill loops
 * avoid repeated shared-cartridge-bus latency during the render critical path. */
#define SHA_SPRITE_CACHE_BYTES 49152u
static uint8_t sprite_cache[SHA_SPRITE_CACHE_BYTES] __attribute__((aligned(16)));
static int16_t sprite_cache_map[SHA_SPRITE_COUNT]; /* -1 = not cached */
static uint8_t sprite_cache_ready;

static void copy_aligned(void *destination, const void *source, uint32_t bytes);

static void sprite_cache_add(uint16_t id, uint32_t *cursor)
{
    const uint8_t *p;
    uint32_t off;
    uint32_t bytes;
    if (id >= SHA_SPRITE_COUNT) return;
    if (sprite_cache_map[id] >= 0) return;
    p = sha_ptr(SHA_SPRITE_META_OFF) + (uint32_t)id * 20u;
    off = sha_rd32(p);
    bytes = (uint32_t)sha_rd16(p + 4) * (uint32_t)sha_rd16(p + 6);
    if (*cursor + bytes > SHA_SPRITE_CACHE_BYTES) return; /* no room */
    copy_aligned(sprite_cache + *cursor, sha_ptr(off), bytes);
    sprite_cache_map[id] = (int16_t)*cursor;
    *cursor = (*cursor + bytes + 15u) & ~15u;
}

static void build_sprite_cache(uint8_t track)
{
    /* Priority is HUD (drawn every frame) > active-track obstacles (frequent,
     * many visible) > particle effects (sporadic, only during collisions).
     * HUD digit fonts are MFBG0-9=41..50, MFBW0-9=51..60, MFMG0-9=61..70,
     * MFMW0-9=71..80. */
    static const uint16_t hud_glyphs[] = {
        SHSPR_MLAPS_IS2, SHSPR_MFBGB_IS2, SHSPR_MFMG0_IS2,
        SHSPR_MPOS_IS2, SHSPR_MFBW0_IS2, SHSPR_MPOSBAR_IS2, SHSPR_MFMW0_IS2,
        SHSPR_MREVO0_IS2, SHSPR_MREVO1_IS2, SHSPR_RACE_0_IS2, SHSPR_RACE_1_IS2,
        SHSPR_RACE_2_IS2, SHSPR_RACE_3_IS2, SHSPR_PAUSE_IS2, SHSPR_YOUWIN_IS2,
        SHSPR_ENDRACE_IS2, SHSPR_MFMGB_IS2
    };
    uint32_t cursor = 0;
    unsigned i;
    const uint8_t *ob;
    for (i = 0; i < SHA_SPRITE_COUNT; ++i) sprite_cache_map[i] = -1;
    sprite_cache_ready = 1;
    copy_aligned(sprite_meta_cache, sha_ptr(SHA_SPRITE_META_OFF),
                 sizeof(sprite_meta_cache));
    /* Priority 1: every-frame HUD glyphs + the four 10-digit fonts. */
    for (i = 0; i < sizeof(hud_glyphs) / sizeof(hud_glyphs[0]); ++i)
        sprite_cache_add(hud_glyphs[i], &cursor);
    for (i = 41; i <= 80; ++i)
        sprite_cache_add((uint16_t)i, &cursor);
    /* Priority 2: active track's trackside obstacle sprites. */
    ob = sha_ptr(track ? SHA_MAP1_OBSTACLES_OFF : SHA_MAP0_OBSTACLES_OFF);
    for (i = 0; i < (track ? SHA_MAP1_OBSTACLE_COUNT : SHA_MAP0_OBSTACLE_COUNT); ++i)
        sprite_cache_add(sha_rd16(ob + i * 12u + 10u), &cursor);
    /* Priority 3: particle effects. */
    {
        static const uint16_t effects[] = {
            SHSPR_SPRK01AA_IS2, SHSPR_SPRK02AA_IS2, SHSPR_SPRK03AA_IS2,
            SHSPR_SPRK04AA_IS2, SHSPR_SPRK05AA_IS2, SHSPR_SPRK06AA_IS2,
            SHSPR_GND001AA_IS2, SHSPR_GND002AA_IS2, SHSPR_GND003AA_IS2,
            SHSPR_GND004AA_IS2, SHSPR_GND005AA_IS2, SHSPR_GND006AA_IS2,
            SHSPR_GND101AA_IS2, SHSPR_GND102AA_IS2, SHSPR_GND103AA_IS2,
            SHSPR_GND104AA_IS2, SHSPR_GND105AA_IS2, SHSPR_GND106AA_IS2,
            SHSPR_GND201AA_IS2, SHSPR_GND202AA_IS2, SHSPR_GND203AA_IS2,
            SHSPR_GND204AA_IS2, SHSPR_GND205AA_IS2, SHSPR_GND206AA_IS2
        };
        for (i = 0; i < sizeof(effects) / sizeof(effects[0]); ++i)
            sprite_cache_add(effects[i], &cursor);
    }
}

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
        out->sector_vertices = sha_ptr(SHA_MAP0_SECTOR_VERTICES_OFF);
        out->sectors = sha_ptr(SHA_MAP0_SECTORS_OFF);
        out->sector_sides = sha_ptr(SHA_MAP0_SECTOR_SIDES_OFF);
        out->sector_object_meta = sha_ptr(SHA_MAP0_SECTOR_OBJECT_META_OFF);
        out->sector_object_indices = sha_ptr(SHA_MAP0_SECTOR_OBJECT_INDICES_OFF);
        out->default_object_bin_meta = sha_ptr(SHA_MAP0_DEFAULT_OBJECT_BIN_META_OFF);
        out->default_object_bin_indices = sha_ptr(SHA_MAP0_DEFAULT_OBJECT_BIN_INDICES_OFF);
        out->obstacles = sha_ptr(SHA_MAP0_OBSTACLES_OFF);
        out->tile_count = SHA_MAP0_TILE_COUNT;
        out->path_count = SHA_MAP0_PATH_COUNT;
        out->start_count = SHA_MAP0_START_COUNT;
        out->camera_count = SHA_MAP0_CAMERA_COUNT;
        out->wall_count = SHA_MAP0_WALL_COUNT;
        out->obstacle_count = SHA_MAP0_OBSTACLE_COUNT;
        out->sector_vertex_count = SHA_MAP0_SECTOR_VERTEX_COUNT;
        out->sector_count = SHA_MAP0_SECTOR_COUNT;
        out->sector_side_count = SHA_MAP0_SECTOR_SIDE_COUNT;
        out->sector_object_bucket_count = SHA_MAP0_SECTOR_OBJECT_BUCKET_COUNT;
    } else {
        out->map = sha_ptr(SHA_MAP1_MAP128_OFF);
        out->tiles = sha_ptr(SHA_MAP1_TILES_OFF);
        out->sky = sha_ptr(SHA_MAP1_SKY_OFF);
        out->mountains = sha_ptr(SHA_MAP1_MOUNTAINS_OFF);
        out->path = sha_ptr(SHA_MAP1_PATH_OFF);
        out->starts = sha_ptr(SHA_MAP1_STARTS_OFF);
        out->cameras = sha_ptr(SHA_MAP1_CAMERAS_OFF);
        out->walls = sha_ptr(SHA_MAP1_WALLS_OFF);
        out->sector_vertices = sha_ptr(SHA_MAP1_SECTOR_VERTICES_OFF);
        out->sectors = sha_ptr(SHA_MAP1_SECTORS_OFF);
        out->sector_sides = sha_ptr(SHA_MAP1_SECTOR_SIDES_OFF);
        out->sector_object_meta = sha_ptr(SHA_MAP1_SECTOR_OBJECT_META_OFF);
        out->sector_object_indices = sha_ptr(SHA_MAP1_SECTOR_OBJECT_INDICES_OFF);
        out->default_object_bin_meta = sha_ptr(SHA_MAP1_DEFAULT_OBJECT_BIN_META_OFF);
        out->default_object_bin_indices = sha_ptr(SHA_MAP1_DEFAULT_OBJECT_BIN_INDICES_OFF);
        out->obstacles = sha_ptr(SHA_MAP1_OBSTACLES_OFF);
        out->tile_count = SHA_MAP1_TILE_COUNT;
        out->path_count = SHA_MAP1_PATH_COUNT;
        out->start_count = SHA_MAP1_START_COUNT;
        out->camera_count = SHA_MAP1_CAMERA_COUNT;
        out->wall_count = SHA_MAP1_WALL_COUNT;
        out->obstacle_count = SHA_MAP1_OBSTACLE_COUNT;
        out->sector_vertex_count = SHA_MAP1_SECTOR_VERTEX_COUNT;
        out->sector_count = SHA_MAP1_SECTOR_COUNT;
        out->sector_side_count = SHA_MAP1_SECTOR_SIDE_COUNT;
        out->sector_object_bucket_count = SHA_MAP1_SECTOR_OBJECT_BUCKET_COUNT;
    }
}

void sha_prepare_track(uint8_t track)
{
    SHTrackAssets raw;
    uint32_t cached_tiles;
    uint32_t sky_bytes, default_objects;
    track &= 1u;
    if (active_track == track) return;

    sha_get_track_raw(track, &raw);
    cached_tiles = raw.tile_count < SHA_TILE_CACHE_COUNT ?
                   raw.tile_count : SHA_TILE_CACHE_COUNT;
    sky_bytes = track ? SHA_MAP1_SKY_SIZE : SHA_MAP0_SKY_SIZE;
    default_objects = track ? SHA_MAP1_DEFAULT_OBJECT_COUNT : SHA_MAP0_DEFAULT_OBJECT_COUNT;

    copy_aligned(track_map_cache, raw.map, sizeof(track_map_cache));
    copy_aligned(tile_cache, raw.tiles, cached_tiles * 4096u);
    copy_aligned(sky_cache, raw.sky, sky_bytes);
    /* Compose the mountains over the bottom of the 70-row sky band in place
     * (read straight from ROM; only this one-time pass touches them). Both pan
     * by the same offset, so a single opaque composite replaces the two-pass
     * sky+mountain draw and removes the per-pixel transparency path. */
    {
        uint32_t mountain_h = track ? 35u : 49u;
        uint32_t y, x;
        for (y = 0; y < mountain_h; ++y) {
            uint8_t *dst = sky_cache + (70u - mountain_h + y) * 320u;
            const uint8_t *src = raw.mountains + y * 320u;
            for (x = 0; x < 320u; ++x)
                if (src[x]) dst[x] = src[x];
        }
    }
    copy_aligned(sector_vertex_cache, raw.sector_vertices,
                 (uint32_t)raw.sector_vertex_count * 4u);
    copy_aligned(sector_cache, raw.sectors,
                 (uint32_t)raw.sector_count * 12u);
    copy_aligned(sector_side_cache, raw.sector_sides,
                 (uint32_t)raw.sector_side_count * 8u);
    copy_aligned(wall_cache, raw.walls, (uint32_t)raw.wall_count * 20u);
    copy_aligned(obstacle_cache, raw.obstacles,
                 (uint32_t)raw.obstacle_count * 12u);
    copy_aligned(sector_object_meta_cache, raw.sector_object_meta,
                 (uint32_t)raw.sector_object_bucket_count * 4u);
    copy_aligned(sector_object_index_cache, raw.sector_object_indices,
                 ((uint32_t)raw.obstacle_count * 2u + 3u) & ~3u);
    copy_aligned(default_object_bin_meta_cache, raw.default_object_bin_meta,
                 sizeof(default_object_bin_meta_cache));
    copy_aligned(default_object_bin_index_cache, raw.default_object_bin_indices,
                 (default_objects * 2u + 3u) & ~3u);
    if (!color_map_ready) {
        copy_aligned(color_map_cache, sha_ptr(SHA_COLOR_MAP_OFF), sizeof(color_map_cache));
        color_map_ready = 1;
    }
    build_sprite_cache(track);
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

/* Build the packed shade table from the color map. Shade level 0..15 maps to
 * color-map rows 31..16. Row content is the byte value expanded to 32-bit
 * 4x-packed pixels so the floor kernel can fetch a color with one load. */
static void build_packed_shade(void)
{
    unsigned row, c;
    if (!color_map_ready) {
        copy_aligned(color_map_cache, sha_ptr(SHA_COLOR_MAP_OFF), sizeof(color_map_cache));
        color_map_ready = 1;
    }
    for (row = 16; row < 32; ++row) {
        const uint8_t *src = color_map_cache + row * 256u;
        uint32_t *dst = packed_shade_cache + (row - 16u) * 256u;
        for (c = 0; c < 256; ++c) {
            uint8_t v = src[c];
            dst[c] = (uint32_t)v * 0x01010101u;
        }
    }
    packed_shade_ready = 1;
}

/* Return a cache-through pointer to the packed shade row for a 0..15 level. */
const uint8_t *sha_packed_shade_row(int level)
{
    if (!packed_shade_ready) build_packed_shade();
    if (level < 0) level = 0;
    if (level > 15) level = 15;
    return (const uint8_t *)CACHE_THROUGH(
        packed_shade_cache + (uint32_t)(15 - level) * 256u);
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
        out->sector_vertices = CACHE_THROUGH(sector_vertex_cache);
        out->sectors = CACHE_THROUGH(sector_cache);
        out->sector_sides = CACHE_THROUGH(sector_side_cache);
        out->sector_object_meta = CACHE_THROUGH(sector_object_meta_cache);
        out->sector_object_indices = CACHE_THROUGH(sector_object_index_cache);
        out->default_object_bin_meta = CACHE_THROUGH(default_object_bin_meta_cache);
        out->default_object_bin_indices = CACHE_THROUGH(default_object_bin_index_cache);
        out->walls = CACHE_THROUGH(wall_cache);
        out->obstacles = CACHE_THROUGH(obstacle_cache);
    }
}

void sha_get_sprite(uint16_t id, SHSprite *out)
{
    const uint8_t *p = sprite_cache_ready
        ? CACHE_THROUGH(sprite_meta_cache + (uint32_t)id * 20u)
        : sha_ptr(SHA_SPRITE_META_OFF) + (uint32_t)id * 20u;
    int16_t off;
    if (sprite_cache_ready && id < SHA_SPRITE_COUNT &&
        (off = sprite_cache_map[id]) >= 0) {
        out->pixels = CACHE_THROUGH(sprite_cache + off);
    } else {
        out->pixels = sha_ptr(sha_rd32(p));
    }
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

void sha_get_sector(const SHTrackAssets *assets, uint16_t id, SHSector *out)
{
    const uint8_t *p;
    if (id >= assets->sector_count) id = 0;
    p = assets->sectors + (uint32_t)id * 12u;
    out->first_side = sha_rd16(p);
    out->side_count = p[2];
    out->flags = p[3];
    out->min_x = sha_rd16(p + 4);
    out->min_y = sha_rd16(p + 6);
    out->max_x = sha_rd16(p + 8);
    out->max_y = sha_rd16(p + 10);
}

void sha_get_sector_side(const SHTrackAssets *assets, uint16_t id, SHSectorSide *out)
{
    const uint8_t *p;
    if (id >= assets->sector_side_count) id = 0;
    p = assets->sector_sides + (uint32_t)id * 8u;
    out->v0 = sha_rd16(p);
    out->v1 = sha_rd16(p + 2);
    out->other = sha_rds16(p + 4);
    out->wall = sha_rd16(p + 6);
}

void sha_get_sector_vertex(const SHTrackAssets *assets, uint16_t id,
                           SHSectorVertex *out)
{
    const uint8_t *p;
    if (id >= assets->sector_vertex_count) id = 0;
    p = assets->sector_vertices + (uint32_t)id * 4u;
    out->x = sha_rd16(p);
    out->y = sha_rd16(p + 2);
}

void sha_get_sector_object_range(const SHTrackAssets *assets, uint16_t sector,
                                 SHSectorObjectRange *out)
{
    const uint8_t *p;
    if (sector >= assets->sector_object_bucket_count)
        sector = (uint16_t)(assets->sector_object_bucket_count - 1u);
    p = assets->sector_object_meta + (uint32_t)sector * 4u;
    out->first = sha_rd16(p);
    out->count = sha_rd16(p + 2);
}

uint16_t sha_get_sector_object_index(const SHTrackAssets *assets, uint16_t id)
{
    if (id >= assets->obstacle_count) id = 0;
    return sha_rd16(assets->sector_object_indices + (uint32_t)id * 2u);
}

void sha_get_default_object_bin_range(const SHTrackAssets *assets, uint16_t bin,
                                      SHSectorObjectRange *out)
{
    const uint8_t *p = assets->default_object_bin_meta + ((uint32_t)bin & 255u) * 4u;
    out->first = sha_rd16(p);
    out->count = sha_rd16(p + 2);
}

uint16_t sha_get_default_object_bin_index(const SHTrackAssets *assets, uint16_t id)
{
    return sha_rd16(assets->default_object_bin_indices + (uint32_t)id * 2u);
}

int sha_sector_contains(const SHTrackAssets *assets, int16_t sector,
                        uint32_t world_x, uint32_t world_y)
{
    SHSector current;
    uint32_t x = (world_x - 0x40000000u) >> 15;
    uint32_t y = (world_y - 0x40000000u) >> 15;
    unsigned i, hits = 0;
    if (sector < 0 || (uint16_t)sector >= assets->sector_count ||
        x > 0xFFFFu || y > 0xFFFFu) return 0;
    sha_get_sector(assets, (uint16_t)sector, &current);
    if (x < current.min_x || x > current.max_x ||
        y < current.min_y || y > current.max_y) return 0;

    /* sectors.c::SEC_IsInSector(): cast a ray in Y and retain its exact
     * half-open X convention so shared polygon edges have one owner. */
    for (i = 0; i < current.side_count; ++i) {
        SHSectorSide side;
        SHSectorVertex v0, v1;
        int32_t iy;
        sha_get_sector_side(assets, current.first_side + i, &side);
        sha_get_sector_vertex(assets, side.v0, &v0);
        sha_get_sector_vertex(assets, side.v1, &v1);
        if (!((x >= v0.x && x < v1.x) || (x < v0.x && x >= v1.x)))
            continue;
        if (v0.y < y && v1.y < y) {
            ++hits;
        } else if (v0.y < y || v1.y < y) {
            if (v0.x < v1.x)
                iy = v0.y + sh2_muldiv((int32_t)x - v0.x,
                                       (int32_t)v1.y - v0.y,
                                       (int32_t)v1.x - v0.x);
            else
                iy = v1.y + sh2_muldiv((int32_t)x - v1.x,
                                       (int32_t)v0.y - v1.y,
                                       (int32_t)v0.x - v1.x);
            if (iy < (int32_t)y) ++hits;
        }
    }
    return (hits & 1u) != 0;
}

int16_t sha_find_sector(const SHTrackAssets *assets, int16_t hint,
                        uint32_t world_x, uint32_t world_y)
{
    unsigned i;
    if (sha_sector_contains(assets, hint, world_x, world_y)) return hint;

    /* Normal movement can only enter a polygon adjacent to the current one.
     * Try those first; the full SEC_FindSector scan remains as a teleport,
     * spawn and malformed-jump fallback. */
    if (hint >= 0 && (uint16_t)hint < assets->sector_count) {
        SHSector current;
        sha_get_sector(assets, (uint16_t)hint, &current);
        for (i = 0; i < current.side_count; ++i) {
            SHSectorSide side;
            sha_get_sector_side(assets, current.first_side + i, &side);
            if (side.other >= 0 && side.other != hint &&
                sha_sector_contains(assets, side.other, world_x, world_y))
                return side.other;
        }
    }
    for (i = 0; i < assets->sector_count; ++i)
        if ((int16_t)i != hint &&
            sha_sector_contains(assets, (int16_t)i, world_x, world_y))
            return (int16_t)i;
    return -1;
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
