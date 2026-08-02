/*
 * Speed Haste software renderer for the 32X framebuffer.
 * Floor/camera formulas follow game/3dfloor.c and game/fla.asm; object and
 * camera transforms follow game/flsprs.c and game/race.c.
 */
#include "sh_render.h"
#include "sh_assets.h"
#include "platform/platform.h"
#include "platform/mars.h"
#include "platform/sh2_math.h"
#include "sh_render_worker.h"

#define VIEW_Y 12
#define VIEW_H 200
#define SKY_H 70
#define CX 160
/*
 * race.c passes hsk + y (the first floor scanline), not screen center, to
 * SEC_Render/FSP. Ground, walls, sprites and cars share this projection base.
 */
#define PROJ_Y (VIEW_Y + SKY_H)
#define SCREEN_CY (VIEW_Y + VIEW_H / 2)
#define MAX_VISIBLE 16
#define MAX_VISIBLE_SECTORS 20
#define MAX_VISIBLE_WALLS 128
/* Far-row decimation is disabled: 8-px columns were too noticeable at the
 * horizon. The whole floor renders at clean 4-px. (Keep the constant so the
 * code path can be re-enabled if a faster floor is ever needed.) */
#define FLOOR_FAR_ROWS 0

typedef struct SHRenderProfile {
    uint16_t visibility, panorama, floor_slave, floor_wait;
    uint16_t walls, obstacles, cars_effects, hud, total;
    uint16_t sector_count, wall_count, object_candidates;
} SHRenderProfile;

static SHRenderProfile render_profile;

static int32_t abs32(int32_t v) { return v < 0 ? -v : v; }
static int32_t mul30(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 30); }
static inline __attribute__((always_inline)) int32_t muldiv(int32_t a, int32_t b, int32_t c)
{
    return c ? sh2_muldiv(a, b, c) : 0;
}

static void fill(volatile uint8_t *fb, uint8_t color)
{
    int i;
    uint32_t pattern = (uint32_t)color * 0x01010101u;
    volatile uint32_t *dst = (volatile uint32_t *)fb;
    for (i = 0; i < 320 * 224 / 4; ++i) dst[i] = pattern;
}

static void rect(volatile uint8_t *fb, int x, int y, int w, int h, uint8_t color)
{
    int yy, xx;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > 320) w = 320 - x;
    if (y + h > 224) h = 224 - y;
    if (w <= 0 || h <= 0) return;
    for (yy = 0; yy < h; ++yy)
        for (xx = 0; xx < w; ++xx)
            fb[(y + yy) * 320 + x + xx] = color;
}

static inline __attribute__((always_inline)) uint8_t floor_pixel(
    const uint8_t *map, const uint8_t *tiles, const uint8_t *cached_tiles,
    uint16_t cached_count, uint32_t x, uint32_t y)
{
    uint32_t gx = x >> 25, gy = y >> 25;
    const uint8_t *pixels;
    uint8_t tile;
    if (gx >= 128 || gy >= 128) return 0;
    tile = map[(gy << 7) + gx];
    pixels = tile < cached_count ? cached_tiles + ((uint32_t)tile << 12) :
                                  tiles + ((uint32_t)tile << 12);
    return pixels[(((y >> 19) & 63u) << 6) + ((x >> 19) & 63u)];
}

/* A rectangular screen band a view is drawn into. Split screen uses two
 * stacked viewports (each ~half height); single player uses the full one. */
typedef struct Viewport {
    int16_t y;      /* screen row of the viewport top edge */
    int16_t h;      /* viewport height in rows */
    int16_t sky_h;  /* sky/panorama band height */
    int16_t proj_y; /* y + sky_h : floor start / projection base row */
    int16_t cx;     /* horizontal centre */
    int16_t cy;     /* y + h/2 : vertical centre */
} Viewport;

typedef struct Camera {
    uint32_t x, y, target_x, target_y;
    uint16_t angle;
    int32_t radius;
    int32_t height, focus, horizon;
    uint8_t cockpit, track;
    uint8_t visible_sector_count, visible_wall_count;
    int16_t visible_sectors[MAX_VISIBLE_SECTORS];
    uint16_t visible_walls[MAX_VISIBLE_WALLS];
    const SHCar *view_car;   /* the car this viewport belongs to (HUD/race) */
    int16_t sector_hint;     /* sector hint for build_visible_sectors */
    Viewport vp;
    SHTrackAssets assets;
} Camera;

static Viewport full_viewport(void)
{
    Viewport v;
    v.y = 12; v.h = 200; v.sky_h = 70;
    v.proj_y = 12 + 70; v.cx = 160; v.cy = 12 + 100;
    return v;
}

static Viewport split_viewport(int bottom)
{
    /* Two stacked 320x100 viewports over the 224-row screen: top 12..111,
     * bottom 112..211. Each keeps the 35-row sky band (proportions halved). */
    Viewport v;
    v.y = bottom ? 112 : 12;
    v.h = 100; v.sky_h = 35;
    v.proj_y = v.y + 35; v.cx = 160; v.cy = v.y + 50;
    return v;
}

static void make_camera(const SHGame *game, const SHCar *p, int16_t sector_hint,
                        Viewport vp, Camera *cam)
{
    cam->track = game->selected_track & 1u;
    sha_get_track(cam->track, &cam->assets);
    cam->target_x = p->x;
    cam->target_y = p->y;
    cam->angle = game->camera == 1 ? p->angle : game->camera_angle;
    cam->cockpit = 0;
    cam->view_car = p;
    cam->sector_hint = sector_hint;
    cam->vp = vp;
    if (game->camera == 3 && cam->assets.camera_count) {
        SHStaticCamera selected = {0, 0, 0};
        uint32_t best = 0xFFFFFFFFu;
        unsigned i;
        for (i = 0; i < cam->assets.camera_count; ++i) {
            SHStaticCamera candidate;
            int32_t dx, dy;
            uint32_t distance;
            sha_get_camera(cam->track, (uint16_t)i, &candidate);
            dx = ((int32_t)(p->x - candidate.x)) >> 20;
            dy = ((int32_t)(p->y - candidate.y)) >> 20;
            distance = (uint32_t)(dx * dx + dy * dy);
            if (distance < best) { best = distance; selected = candidate; }
        }
        {
            int32_t dx = ((int32_t)(p->x - selected.x)) >> 20;
            int32_t dy = ((int32_t)(p->y - selected.y)) >> 20;
            uint32_t ax = (uint32_t)abs32(dx), ay = (uint32_t)abs32(dy);
            uint32_t distance = (ax > ay ? ax : ay) + ((ax > ay ? ay : ax) >> 1);
            cam->x = selected.x;
            cam->y = selected.y;
            cam->angle = sha_vector_angle(dx, -dy);
            cam->radius = 0;
            cam->height = (int32_t)selected.height;
            cam->horizon = cam->height / 2000 + 1;
            cam->focus = 0x0A00 + muldiv(0x1000, (int32_t)distance, 256);
            return;
        }
    }
    if (game->camera == 1) {
        cam->radius = 0;
        cam->height = 1100;
        cam->focus = 0x0BE0;
        cam->horizon = 1;
        cam->cockpit = 1;
    } else if (game->camera == 2) {
        cam->radius = 40137344;
        cam->height = 2976;
        cam->focus = 3200;
        cam->horizon = 1;
    } else {
        cam->radius = 20165824;
        cam->height = 1848;
        cam->focus = 3136;
        cam->horizon = 1;
    }
    cam->x = p->x - (uint32_t)mul30(cam->radius, sha_cos30(cam->angle));
    cam->y = p->y - (uint32_t)mul30(cam->radius, sha_sin30(cam->angle));
}

static int portal_may_be_visible(const Camera *cam, const SHSectorSide *side)
{
    SHSectorVertex v0, v1;
    int32_t ca = sha_cos30(cam->angle), sa = sha_sin30(cam->angle);
    int32_t x0, y0, x1, y1, d0, d1, s0, s1;
    sha_get_sector_vertex(&cam->assets, side->v0, &v0);
    sha_get_sector_vertex(&cam->assets, side->v1, &v1);
    x0 = (int32_t)((((uint32_t)v0.x << 15) + 0x40000000u) - cam->x) >> 6;
    y0 = (int32_t)((((uint32_t)v0.y << 15) + 0x40000000u) - cam->y) >> 6;
    x1 = (int32_t)((((uint32_t)v1.x << 15) + 0x40000000u) - cam->x) >> 6;
    y1 = (int32_t)((((uint32_t)v1.y << 15) + 0x40000000u) - cam->y) >> 6;
    d0 = mul30(x0, ca) + mul30(y0, sa);
    d1 = mul30(x1, ca) + mul30(y1, sa);
    if (d0 < (8 << 12) && d1 < (8 << 12)) return 0;
    s0 = mul30(y0, ca) - mul30(x0, sa);
    s1 = mul30(y1, ca) - mul30(x1, sa);
    /* Conservative 120-degree portal cone. A side with one endpoint behind
     * remains eligible, avoiding holes at bends and near the chase camera. */
    if (d0 > 0 && d1 > 0 && s0 < -(d0 << 1) && s1 < -(d1 << 1)) return 0;
    if (d0 > 0 && d1 > 0 && s0 >  (d0 << 1) && s1 >  (d1 << 1)) return 0;
    return 1;
}

static void build_visible_sectors(Camera *cam)
{
    uint32_t seen_sectors[3] = {0, 0, 0};
    uint32_t seen_walls[4] = {0, 0, 0, 0};
    int16_t start = sha_find_sector(&cam->assets, cam->sector_hint,
                                    cam->x, cam->y);
    unsigned head = 0, i;
    cam->visible_sector_count = 0;
    cam->visible_wall_count = 0;
    if (start < 0) start = cam->sector_hint;
    if (start < 0 || (uint16_t)start >= cam->assets.sector_count) return;
    cam->visible_sectors[cam->visible_sector_count++] = start;
    seen_sectors[(uint16_t)start >> 5] |= 1u << ((uint16_t)start & 31u);

    /* sectors.c::SEC_Render performs a bounded breadth-first adjacency walk.
     * Retain that source convention and add a conservative portal cone. */
    while (head < cam->visible_sector_count &&
           cam->visible_sector_count < MAX_VISIBLE_SECTORS) {
        SHSector sector;
        int16_t sector_id = cam->visible_sectors[head++];
        sha_get_sector(&cam->assets, (uint16_t)sector_id, &sector);
        for (i = 0; i < sector.side_count; ++i) {
            SHSectorSide side;
            sha_get_sector_side(&cam->assets, sector.first_side + i, &side);
            if (side.wall != 0xFFFFu && side.wall < cam->assets.wall_count &&
                !(seen_walls[side.wall >> 5] & (1u << (side.wall & 31u)))) {
                seen_walls[side.wall >> 5] |= 1u << (side.wall & 31u);
                if (cam->visible_wall_count < MAX_VISIBLE_WALLS)
                    cam->visible_walls[cam->visible_wall_count++] = side.wall;
            }
            if (side.other >= 0 && (uint16_t)side.other < cam->assets.sector_count &&
                !(seen_sectors[(uint16_t)side.other >> 5] &
                  (1u << ((uint16_t)side.other & 31u))) &&
                portal_may_be_visible(cam, &side)) {
                seen_sectors[(uint16_t)side.other >> 5] |=
                    1u << ((uint16_t)side.other & 31u);
                cam->visible_sectors[cam->visible_sector_count++] = side.other;
                if (cam->visible_sector_count >= MAX_VISIBLE_SECTORS) break;
            }
        }
    }

    /* Preserve the old global-wall order to avoid changing painter overlap. */
    for (i = 1; i < cam->visible_wall_count; ++i) {
        uint16_t wall = cam->visible_walls[i];
        unsigned j = i;
        while (j && cam->visible_walls[j - 1] > wall) {
            cam->visible_walls[j] = cam->visible_walls[j - 1];
            --j;
        }
        cam->visible_walls[j] = wall;
    }
}

static void draw_background(volatile uint8_t *fb, const Camera *cam)
{
    /* sha_prepare_track composites the mountains over the sky band, so sky
     * is already fully opaque and pans as a single image. No per-pixel
     * transparency and no separate mountain pass remain. */
    const uint8_t *sky = cam->assets.sky;
    const int sky_h = cam->vp.sky_h;
    const int vp_y = cam->vp.y;
    /* The composite panorama is 70 rows; a smaller split viewport sky band
     * samples it (stride) to keep both sky and mountains visible. */
    const int pano_h = 70;
    const int sstep = (pano_h > sky_h && sky_h > 0) ? (pano_h / sky_h) : 1;
    int offset = (int)(((uint32_t)3 * cam->angle * 320u) >> 16);
    int y, x;
    while (offset >= 320) offset -= 320;
    for (y = 0; y < sky_h; ++y) {
        const uint8_t *src = sky + (y * sstep) * 320;
        volatile uint32_t *dst = (volatile uint32_t *)(fb + (vp_y + y) * 320);
        int sx = 320 - offset;
        if (sx == 320) sx = 0;
        /* Four pixels per aligned write. Only one group per row can straddle
         * the panorama seam; all other groups read four contiguous bytes. */
        for (x = 0; x < 80; ++x) {
            uint32_t pattern;
            if (sx <= 316) {
                pattern = ((uint32_t)src[sx] << 24) |
                          ((uint32_t)src[sx + 1] << 16) |
                          ((uint32_t)src[sx + 2] << 8) | src[sx + 3];
                sx += 4;
                if (sx == 320) sx = 0;
            } else {
                uint8_t c0 = src[sx]; if (++sx == 320) sx = 0;
                uint8_t c1 = src[sx]; if (++sx == 320) sx = 0;
                uint8_t c2 = src[sx]; if (++sx == 320) sx = 0;
                uint8_t c3 = src[sx]; if (++sx == 320) sx = 0;
                pattern = ((uint32_t)c0 << 24) | ((uint32_t)c1 << 16) |
                          ((uint32_t)c2 << 8) | c3;
            }
            dst[x] = pattern;
        }
    }
}

static void draw_floor(volatile uint8_t *fb, const Camera *cam)
{
    const uint8_t *map = cam->assets.map;
    const uint8_t *tiles = cam->assets.tiles;
    const uint8_t *cached_tiles = cam->assets.cached_tiles;
    const uint16_t cached_count = cam->assets.cached_tile_count;
    const int floor_y = cam->vp.proj_y;
    const int floor_rows = cam->vp.h - cam->vp.sky_h;
    SH2FloorRowJob floor_job;
    int row;
    floor_job.map = map;
    floor_job.tiles = tiles;
    floor_job.cached_tiles = cached_tiles;
    floor_job.cached_count = cached_count;
    /* The 32X cartridge bus is shared by both SH-2s. Render the floor at
     * 80x65 samples and expand 4x2, matching the chunky low-detail option of
     * the DOS game while reducing ROM tile fetches by eight. Far rows (near
     * the horizon) are perspective-compressed, so they use 8-px columns
     * (40 samples) with no visible change, halving their sample count. */
    for (row = 0; row < floor_rows; row += 2) {
        int Y = cam->horizon + row;
        int32_t radius = muldiv(cam->height, cam->focus, Y);
        int level;
        int32_t step_x, step_y, pos_x, pos_y;
        if (radius < (4 << 16)) level = 15;
        else if (radius >= (36 << 16)) level = 0;
        else level = 14 - (int)(((int64_t)14 * (radius - (4 << 16)) / 32) >> 16);
        if (level < 0) level = 0;
        if (level > 15) level = 15;

        step_x = mul30(muldiv(cam->height, 0x1000, Y), sha_cos30((uint16_t)(cam->angle - 16384)));
        step_y = mul30(muldiv(cam->height, 0x1000, Y), sha_sin30((uint16_t)(cam->angle - 16384)));
        pos_x = (int32_t)cam->x + (int32_t)(((int64_t)radius * sha_cos30(cam->angle)) >> 22)
              - (int32_t)(((int64_t)step_x * 320) >> 1);
        pos_y = (int32_t)cam->y + (int32_t)(((int64_t)radius * sha_sin30(cam->angle)) >> 22)
              - (int32_t)(((int64_t)step_y * 320) >> 1);
        floor_job.dst0 = (volatile uint32_t *)(fb + (floor_y + row) * 320);
        floor_job.dst1 = (volatile uint32_t *)(fb + (floor_y + row + 1) * 320);
        floor_job.shade = sha_packed_shade_row(level);
        if (row < FLOOR_FAR_ROWS) {
            /* 8-px columns: center of first column is 4 px in; step is 8 px. */
            pos_x += step_x * 4;
            pos_y += step_y * 4;
            floor_job.pos_x = (uint32_t)pos_x;
            floor_job.pos_y = (uint32_t)pos_y;
            floor_job.step_x = step_x * 8;
            floor_job.step_y = step_y * 8;
            sh2_floor_row_far(&floor_job);
        } else {
            pos_x += step_x + step_x;
            pos_y += step_y + step_y;
            step_x <<= 2;
            step_y <<= 2;
            floor_job.pos_x = (uint32_t)pos_x;
            floor_job.pos_y = (uint32_t)pos_y;
            floor_job.step_x = step_x;
            floor_job.step_y = step_y;
            sh2_floor_row(&floor_job);
        }
    }
}

void sh_render_slave_floor(volatile SHFloorJob *job)
{
    Camera cam;
    uint8_t stamp = platform_profile_ticks();
    cam.x = job->camera_x;
    cam.y = job->camera_y;
    cam.angle = job->angle;
    cam.track = (uint8_t)job->track;
    cam.height = job->height;
    cam.focus = job->focus;
    cam.horizon = job->horizon;
    cam.target_x = cam.target_y = 0;
    cam.radius = 0;
    cam.cockpit = 0;
    cam.vp = full_viewport();   /* slave renders the single-player full floor */
    sha_get_track(cam.track, &cam.assets);
    /* Use the exact pointers submitted by the master; both are cartridge ROM. */
    cam.assets.map = job->map;
    cam.assets.tiles = job->tiles;
    cam.assets.cached_tiles = job->cached_tiles;
    cam.assets.cached_tile_count = job->cached_tile_count;
    draw_floor(job->framebuffer, &cam);
    job->profile_ticks = (uint8_t)(platform_profile_ticks() - stamp);
}

#ifdef ENABLE_DUAL_SH2_RENDER
static void start_slave_floor(volatile uint8_t *fb, const Camera *cam)
{
    volatile SHFloorJob *job = SH_FLOOR_JOB;
    while (MARS_SYS_COMM6 != SH_SLAVE_IDLE) { }
    job->framebuffer = fb;
    job->map = cam->assets.map;
    job->tiles = cam->assets.tiles;
    job->cached_tiles = cam->assets.cached_tiles;
    job->cached_tile_count = (uint8_t)cam->assets.cached_tile_count;
    job->camera_x = cam->x;
    job->camera_y = cam->y;
    job->angle = cam->angle;
    job->track = cam->track;
    job->height = cam->height;
    job->focus = cam->focus;
    job->horizon = cam->horizon;
    job->profile_ticks = 0;
    MARS_SYS_COMM6 = SH_SLAVE_DRAW_FLOOR;
}

static uint16_t wait_slave_floor(void)
{
    uint16_t ticks;
    while (MARS_SYS_COMM6 != SH_SLAVE_FLOOR_DONE) { }
    ticks = SH_FLOOR_JOB->profile_ticks;
    MARS_SYS_COMM6 = SH_SLAVE_IDLE;
    return ticks;
}
#endif

static void draw_sprite_scaled(volatile uint8_t *fb, const SHSprite *sp,
                               int center_x, int base_y, int width, int height,
                               int mirror, const Viewport *vp)
{
    int y, x;
    uint32_t syfp = 0, systep, sxstep;
    if (!width || !height) return;
    if (width < 0) { width = -width; mirror ^= 1; }
    if (width > 240 || height > vp->h) return;
    systep = ((uint32_t)sp->height << 16) / (uint32_t)height;
    sxstep = ((uint32_t)sp->width << 16) / (uint32_t)width;
    systep <<= 1;
    sxstep <<= 1;
    {
        int left = center_x - muldiv(sp->dx, width, sp->width);
        int top = base_y - muldiv(sp->dy, height, sp->height);
        int y0 = vp->y, y1 = vp->y + vp->h;
        /* Fast path: fully on-screen, non-mirrored sprite. Drops the per-pixel
         * dx/dy/mirror tests and the clamp for x+1/dy+1; the common case for
         * trackside objects and particles. */
        if (!mirror && left >= 0 && top >= y0 &&
            left + width <= 320 && top + height <= y1) {
            for (y = 0; y < height; y += 2, syfp += systep) {
                int sy = (int)(syfp >> 16);
                int dy = top + y;
                uint32_t sxfp = 0;
                uint8_t *row0 = (uint8_t *)fb + (uintptr_t)(dy * 320 + left);
                uint8_t *row1 = row0 + 320;
                for (x = 0; x < width; x += 2, sxfp += sxstep) {
                    int sx = (int)(sxfp >> 16);
                    uint8_t c = sp->pixels[sy * sp->width + sx];
                    if (c) {
                        row0[x] = c; row0[x + 1] = c;
                        row1[x] = c; row1[x + 1] = c;
                    }
                }
            }
            return;
        }
        for (y = 0; y < height; y += 2, syfp += systep) {
            int sy = (int)(syfp >> 16);
            int dy = top + y;
            uint32_t sxfp = 0;
            if (dy < y0 || dy >= y1) continue;
            for (x = 0; x < width; x += 2, sxfp += sxstep) {
                int sx = (int)(sxfp >> 16);
                int dx = left + x;
                uint8_t c;
                if (mirror) sx = sp->width - 1 - sx;
                if (dx < 0 || dx >= 320) continue;
                c = sp->pixels[sy * sp->width + sx];
                if (c) {
                    fb[dy * 320 + dx] = c;
                    if (dx + 1 < 320) fb[dy * 320 + dx + 1] = c;
                    if (dy + 1 < y1) {
                        fb[(dy + 1) * 320 + dx] = c;
                        if (dx + 1 < 320) fb[(dy + 1) * 320 + dx + 1] = c;
                    }
                }
            }
        }
    }
}

static void draw_sprite_xy(volatile uint8_t *fb, uint16_t id, int x, int y)
{
    SHSprite sp;
    int yy, xx;
    sha_get_sprite(id, &sp);
    /* Fast path: HUD sprites sit fully on-screen at fixed positions, so the
     * per-pixel bounds checks and per-pixel address recompute are dropped. */
    if (x >= 0 && y >= 0 && x + sp.width <= 320 && y + sp.height <= 224) {
        uint8_t *base = (uint8_t *)fb + (uintptr_t)(y * 320 + x);
        for (yy = 0; yy < sp.height; ++yy) {
            const uint8_t *src = sp.pixels + yy * sp.width;
            uint8_t *dst = base + (uintptr_t)(yy * 320);
            for (xx = 0; xx < sp.width; ++xx) {
                uint8_t c = src[xx];
                if (c) dst[xx] = c;
            }
        }
        return;
    }
    for (yy = 0; yy < sp.height; ++yy) {
        int dy = y + yy;
        if (dy < 0 || dy >= 224) continue;
        for (xx = 0; xx < sp.width; ++xx) {
            int dx = x + xx;
            uint8_t c;
            if (dx < 0 || dx >= 320) continue;
            c = sp.pixels[yy * sp.width + xx];
            if (c) fb[dy * 320 + dx] = c;
        }
    }
}

static void draw_sprite_centered(volatile uint8_t *fb, uint16_t id, int cx, int cy)
{
    SHSprite sp;
    sha_get_sprite(id, &sp);
    /* IS2_Draw treats x/y as the asset hotspot and subtracts dx/dy. */
    draw_sprite_xy(fb, id, cx - sp.dx, cy - sp.dy);
}

/* Small fallback text is used only for menu prompts absent from the data. */
static const uint8_t font[][7] = {
 {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{15,16,16,16,16,16,15},
 {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
 {15,16,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},
 {7,2,2,2,2,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
 {17,27,21,21,17,17,17},{17,25,25,21,19,19,17},{14,17,17,17,17,17,14},
 {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
 {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
 {17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
 {17,17,10,4,4,4,4},{31,1,2,4,8,16,31}
};

static int text_width(const char *s)
{
    int width = 0;
    while (*s++) width += 6;
    return width ? width - 1 : 0;
}

static void text(volatile uint8_t *fb, int x, int y, const char *s, uint8_t color)
{
    while (*s) {
        char c = *s++;
        int yy, xx;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c >= 'A' && c <= 'Z') {
            const uint8_t *g = font[c - 'A'];
            for (yy = 0; yy < 7; ++yy)
                for (xx = 0; xx < 5; ++xx)
                    if (g[yy] & (16 >> xx)) rect(fb, x + xx, y + yy, 1, 1, color);
        }
        x += 6;
    }
}

static void __attribute__((unused)) polygon(volatile uint8_t *fb, const int16_t *px, const int16_t *py, int count, uint8_t color)
{
    int miny = 223, maxy = 0, i, y;
    for (i = 0; i < count; ++i) {
        if (py[i] < miny) miny = py[i];
        if (py[i] > maxy) maxy = py[i];
    }
    if (miny < VIEW_Y) miny = VIEW_Y;
    if (maxy >= VIEW_Y + VIEW_H) maxy = VIEW_Y + VIEW_H - 1;
    for (y = miny; y <= maxy; y += 2) {
        int hits[16], nh = 0;
        for (i = 0; i < count; ++i) {
            int j = (i + 1) % count;
            if ((py[i] <= y && py[j] > y) || (py[j] <= y && py[i] > y)) {
                int x = px[i] + (int32_t)(y - py[i]) * (px[j] - px[i]) / (py[j] - py[i]);
                if (nh < 16) hits[nh++] = x;
            }
        }
        for (i = 1; i < nh; ++i) { int v = hits[i], j = i; while (j && hits[j-1] > v) { hits[j] = hits[j-1]; --j; } hits[j] = v; }
        for (i = 0; i + 1 < nh; i += 2) {
            int x0 = hits[i], x1 = hits[i+1], x;
            if (x0 < 0) x0 = 0;
            if (x1 > 319) x1 = 319;
            for (x = x0; x <= x1; x += 2) {
                fb[y * 320 + x] = color;
                if (x + 1 <= x1) fb[y * 320 + x + 1] = color;
                if (y + 1 <= maxy) {
                    fb[(y + 1) * 320 + x] = color;
                    if (x + 1 <= x1) fb[(y + 1) * 320 + x + 1] = color;
                }
            }
        }
    }
}

typedef struct ProjectedFace { uint8_t count, color; int32_t depth; int16_t x[12], y[12]; } ProjectedFace;

static void __attribute__((unused)) draw_car_model(volatile uint8_t *fb, const Camera *cam, const SHCar *car)
{
    SHModel model;
    int32_t vx[96], vy[96], vz[96];
    int16_t sx[96], sy[96];
    ProjectedFace faces[64];
    int face_count = 0;
    int32_t relx = (int32_t)(car->x - cam->x) >> 6;
    int32_t rely = (int32_t)(car->y - cam->y) >> 6;
    int32_t depth = mul30(relx, sha_cos30(cam->angle)) + mul30(rely, sha_sin30(cam->angle));
    int32_t side = mul30(rely, sha_cos30(cam->angle)) - mul30(relx, sha_sin30(cam->angle));
    uint16_t a = (uint16_t)(0x8000 - cam->angle + car->angle);
    int32_t ca = sha_cos30(a), sa = sha_sin30(a);
    const uint8_t *p;
    unsigned i;
    if (depth < (4 << 14) || depth > (24 << 20) || abs32(side / 2) > depth) return;
    sha_get_model(car->car_type, car->model % 6, &model);
    for (i = 0; i < model.vertex_count; ++i) {
        int32_t x = sha_rds32(model.data + i * 12u);
        int32_t y = sha_rds32(model.data + i * 12u + 4);
        int32_t z = sha_rds32(model.data + i * 12u + 8);
        int32_t dz = (depth >> 4) + mul30(z, ca) - mul30(x, sa);
        vx[i] = ((side >> 4) + mul30(x, ca) + mul30(z, sa)) >> 6;
        vy[i] = (((cam->height << 2) - ((int32_t)car->z >> 10) - y) >> 6);
        vz[i] = dz >> 2;
        if (vz[i] < 64) vz[i] = 64;
        sx[i] = (int16_t)(cam->vp.cx + muldiv(vx[i], cam->focus, vz[i]));
        sy[i] = (int16_t)(cam->vp.proj_y - cam->horizon + muldiv(vy[i], cam->focus, vz[i]));
    }
    p = model.data + (uint32_t)model.vertex_count * 12u;
    for (i = 0; i < model.face_count && face_count < 64; ++i) {
        uint8_t n = p[0], color = p[1];
        ProjectedFace *f = &faces[face_count];
        int32_t area = 0, zsum = 0;
        unsigned j;
        if (n > 12) n = 12;
        f->count = n; f->color = color;
        for (j = 0; j < n; ++j) {
            uint16_t vi = sha_rd16(p + 4 + j * 2u);
            f->x[j] = sx[vi]; f->y[j] = sy[vi]; zsum += vz[vi];
        }
        for (j = 0; j < n; ++j) {
            unsigned k = (j + 1) % n;
            area += f->x[j] * f->y[k] - f->x[k] * f->y[j];
        }
        f->depth = zsum / n;
        if (area < 0 || n == 3) ++face_count;
        p += 4 + ((n * 2 + 3) & ~3);
    }
    for (i = 1; i < (unsigned)face_count; ++i) {
        ProjectedFace v = faces[i]; int j = i;
        while (j && faces[j-1].depth < v.depth) { faces[j] = faces[j-1]; --j; }
        faces[j] = v;
    }
    for (i = 0; i < (unsigned)face_count; ++i)
        polygon(fb, faces[i].x, faces[i].y, faces[i].count, faces[i].color);
}

static void __attribute__((unused)) draw_simple_car(volatile uint8_t *fb, const Camera *cam, const SHCar *car)
{
    int32_t relx = (int32_t)(car->x - cam->x) >> 6;
    int32_t rely = (int32_t)(car->y - cam->y) >> 6;
    int32_t ca = sha_cos30(cam->angle), sa = sha_sin30(cam->angle);
    int32_t depth = mul30(relx, ca) + mul30(rely, sa);
    int32_t side;
    int x, y, w, h;
    uint8_t color;
    if (depth < (4 << 14) || depth > (24 << 20)) return;
    side = mul30(rely, ca) - mul30(relx, sa);
    if (abs32(side / 2) > depth) return;
    x = cam->vp.cx + muldiv(side >> 4, cam->focus, depth);
    y = cam->vp.proj_y + muldiv(cam->height << 2, cam->focus, depth) - cam->horizon;
    w = muldiv(9000, cam->focus, depth); if (w < 2) w = 2;
    h = muldiv(4500, cam->focus, depth); if (h < 2) h = 2;
    color = (uint8_t)(161 + (car->model % 6) * 3);
    rect(fb, x - w / 2, y - h, w, h, color);
    rect(fb, x - w * 2 / 3, y - h, w * 4 / 3, h / 3 + 1, color);
}

static void draw_car_sprite(volatile uint8_t *fb, const Camera *cam, const SHCar *car)
{
    int32_t relx = (int32_t)(car->x - cam->x) >> 6;
    int32_t rely = (int32_t)(car->y - cam->y) >> 6;
    int32_t ca = sha_cos30(cam->angle), sa = sha_sin30(cam->angle);
    int32_t depth = mul30(relx, ca) + mul30(rely, sa);
    int32_t side;
    int x, y, w, h;
    uint16_t view;
    SHSprite sprite;
    if (depth < (4 << 14) || depth > (24 << 20)) return;
    side = mul30(rely, ca) - mul30(relx, sa);
    if (abs32(side / 2) > depth) return;
    x = cam->vp.cx + muldiv(side >> 4, cam->focus, depth);
    y = cam->vp.proj_y + muldiv((cam->height << 2) - ((int32_t)car->z >> 10), cam->focus, depth) - cam->horizon;
    w = muldiv(9000, cam->focus, depth); if (w < 2) w = 2;
    h = w * 3 / 4;
    /* flsprs.c Draw3D uses 0x8000 - camera_angle + object_angle. */
    view = (uint16_t)(((uint16_t)(0x8000u - cam->angle + car->angle + 2048u) >> 12) & 15u);
    sprite.pixels = sha_ptr(SHA_CAR_SPRITES_OFF) +
                    (((uint32_t)(car->car_type & 1u) * 6u + (car->model % 6u)) *
                     16u + view) * (64u * 48u);
    sprite.width = 64; sprite.height = 48; sprite.dx = 32; sprite.dy = 48;
    sprite.world_width = sprite.world_height = 0;
    draw_sprite_scaled(fb, &sprite, x, y, w, h, 0, &cam->vp);
}

static void draw_race_cars(volatile uint8_t *fb, const Camera *cam, const SHGame *game)
{
    /* The original FS3 system supports directional bitmap cars. The importer
     * pre-renders the I3D meshes into that path, eliminating per-frame polygon
     * division while preserving each original model and palette. */
    draw_car_sprite(fb, cam, &game->ai[3]);
    draw_car_sprite(fb, cam, &game->ai[2]);
    draw_car_sprite(fb, cam, &game->ai[1]);
    draw_car_sprite(fb, cam, &game->ai[0]);
    if (!cam->cockpit) draw_car_sprite(fb, cam, &game->player);
    if (game->split) draw_car_sprite(fb, cam, &game->player2);
}

typedef struct VisibleSprite { int32_t depth; int x, y, w, h; uint16_t id; } VisibleSprite;

/* Obstacle candidate work list: collect cheap {depth,index} first, then
 * project and draw only the MAX_VISIBLE nearest. */
#define MAX_CANDIDATES 96

static uint16_t draw_obstacles(volatile uint8_t *fb, const Camera *cam)
{
    uint32_t candidate_mask[11] = {0}; /* enough for 325 shareware objects */
    int32_t candidate_depth[MAX_CANDIDATES];
    uint16_t candidate_index[MAX_CANDIDATES];
    int count = 0;
    int32_t ca = sha_cos30(cam->angle), sa = sha_sin30(cam->angle);
    uint16_t candidates = 0;
    unsigned bucket_index, i;

    /* Mark explicit visible-sector ranges first. */
    for (bucket_index = 0; bucket_index < cam->visible_sector_count; ++bucket_index) {
        SHSectorObjectRange range;
        uint16_t bucket = (uint16_t)cam->visible_sectors[bucket_index];
        sha_get_sector_object_range(&cam->assets, bucket, &range);
        candidates = (uint16_t)(candidates + range.count);
        for (i = 0; i < range.count; ++i) {
            uint16_t id = sha_get_sector_object_index(
                &cam->assets, (uint16_t)(range.first + i));
            candidate_mask[id >> 5] |= 1u << (id & 31u);
        }
    }

    /* SEC_Render always includes its implicit default sector. Preserve that
     * content, but spatially reject its empty/distant 16x16 world bins before
     * touching individual object records. Bin bounds are expanded so large
     * landmarks cannot pop at the 24-unit draw boundary. */
    for (bucket_index = 0; bucket_index < 256; ++bucket_index) {
        SHSectorObjectRange range;
        uint32_t center_x, center_y;
        int32_t relx, rely, depth, side;
        const int32_t margin = 3 << 20;
        sha_get_default_object_bin_range(&cam->assets, (uint16_t)bucket_index, &range);
        if (!range.count) continue;
        center_x = ((uint32_t)(bucket_index & 15u) << 28) + (1u << 27);
        center_y = ((uint32_t)(bucket_index >> 4) << 28) + (1u << 27);
        relx = (int32_t)(center_x - cam->x) >> 6;
        rely = (int32_t)(center_y - cam->y) >> 6;
        if (abs32(relx) > (24 << 20) + margin ||
            abs32(rely) > (24 << 20) + margin) continue;
        depth = mul30(relx, ca) + mul30(rely, sa);
        if (depth < -margin || depth > (24 << 20) + margin) continue;
        side = mul30(rely, ca) - mul30(relx, sa);
        if (depth > 0 && abs32(side) > depth * 3 + margin * 4) continue;
        candidates = (uint16_t)(candidates + range.count);
        for (i = 0; i < range.count; ++i) {
            uint16_t id = sha_get_default_object_bin_index(
                &cam->assets, (uint16_t)(range.first + i));
            candidate_mask[id >> 5] |= 1u << (id & 31u);
        }
    }
    for (i = 0; i < cam->assets.obstacle_count; ++i) {
        const uint8_t *record;
        SHObstacle ob;
        int32_t relx, rely, depth, side;
        if (!(candidate_mask[i >> 5] & (1u << (i & 31u)))) continue;
        record = cam->assets.obstacles + (uint32_t)i * 12u;
        ob.x = sha_rd32(record); ob.y = sha_rd32(record + 4);
        ob.sprite = sha_rd16(record + 10);
        relx = (int32_t)(ob.x - cam->x) >> 6;
        rely = (int32_t)(ob.y - cam->y) >> 6;
        if (abs32(relx) > (24 << 20) || abs32(rely) > (24 << 20)) continue;
        depth = mul30(relx, ca) + mul30(rely, sa);
        if (depth < (4 << 14) || depth > (24 << 20)) continue;
        side = mul30(rely, ca) - mul30(relx, sa);
        if (abs32(side / 3) > depth) continue;
        /* Collect only {depth,index} here; the expensive 3D projection and
         * sprite fetch happen only for the MAX_VISIBLE nearest after sorting. */
        if (count < MAX_CANDIDATES) {
            candidate_depth[count] = depth;
            candidate_index[count] = (uint16_t)i;
            ++count;
        }
    }
    /* Keep the nearest MAX_VISIBLE by insertion sort (descending depth). */
    if (count > MAX_VISIBLE) count = MAX_VISIBLE;
    for (i = 1; i < (unsigned)count; ++i) {
        int32_t d = candidate_depth[i];
        uint16_t idx = candidate_index[i];
        unsigned j = i;
        while (j && candidate_depth[j - 1] < d) {
            candidate_depth[j] = candidate_depth[j - 1];
            candidate_index[j] = candidate_index[j - 1];
            --j;
        }
        candidate_depth[j] = d; candidate_index[j] = idx;
    }
    for (i = 0; i < (unsigned)count; ++i) {
        const uint8_t *record;
        SHObstacle ob; SHSprite sp;
        int32_t relx, rely, side;
        int x, y, w, h;
        int32_t depth = candidate_depth[i];
        record = cam->assets.obstacles + (uint32_t)candidate_index[i] * 12u;
        ob.x = sha_rd32(record); ob.y = sha_rd32(record + 4);
        ob.sprite = sha_rd16(record + 10);
        relx = (int32_t)(ob.x - cam->x) >> 6;
        rely = (int32_t)(ob.y - cam->y) >> 6;
        side = mul30(rely, ca) - mul30(relx, sa);
        sha_get_sprite(ob.sprite, &sp);
        x = cam->vp.cx + muldiv(side >> 4, cam->focus, depth);
        y = cam->vp.proj_y + muldiv(cam->height << 2, cam->focus, depth) - cam->horizon;
        w = muldiv((int32_t)sp.world_width, cam->focus, depth);
        h = muldiv((int32_t)sp.world_height, cam->focus, depth);
        if (w > 0 && h > 0 && w <= 200 && h <= 200)
            draw_sprite_scaled(fb, &sp, x, y, w, h, 0, &cam->vp);
    }
    return candidates;
}

static void draw_effects(volatile uint8_t *fb, const Camera *cam, const SHGame *game)
{
    static const uint16_t spark_frames[6] = {
        SHSPR_SPRK01AA_IS2, SHSPR_SPRK02AA_IS2, SHSPR_SPRK03AA_IS2,
        SHSPR_SPRK04AA_IS2, SHSPR_SPRK05AA_IS2, SHSPR_SPRK06AA_IS2
    };
    static const uint16_t smoke_frames[3][6] = {
        {SHSPR_GND001AA_IS2, SHSPR_GND002AA_IS2, SHSPR_GND003AA_IS2,
         SHSPR_GND004AA_IS2, SHSPR_GND005AA_IS2, SHSPR_GND006AA_IS2},
        {SHSPR_GND101AA_IS2, SHSPR_GND102AA_IS2, SHSPR_GND103AA_IS2,
         SHSPR_GND104AA_IS2, SHSPR_GND105AA_IS2, SHSPR_GND106AA_IS2},
        {SHSPR_GND201AA_IS2, SHSPR_GND202AA_IS2, SHSPR_GND203AA_IS2,
         SHSPR_GND204AA_IS2, SHSPR_GND205AA_IS2, SHSPR_GND206AA_IS2}
    };
    int32_t ca = sha_cos30(cam->angle), sa = sha_sin30(cam->angle);
    unsigned i;
    for (i = 0; i < SH_EFFECTS; ++i) {
        const SHEffect *effect = &game->effects[i];
        int32_t relx, rely, depth, side;
        int x, y;
        if (effect->type == SH_FX_NONE) continue;
        relx = ((int32_t)(effect->x - cam->x)) >> 6;
        rely = ((int32_t)(effect->y - cam->y)) >> 6;
        depth = mul30(relx, ca) + mul30(rely, sa);
        if (depth < (4 << 14) || depth > (20 << 20)) continue;
        side = mul30(rely, ca) - mul30(relx, sa);
        if (abs32(side / 2) > depth) continue;
        x = cam->vp.cx + muldiv(side >> 4, cam->focus, depth);
        y = cam->vp.proj_y + muldiv((cam->height << 2) - ((int32_t)effect->z >> 10),
                            cam->focus, depth) - cam->horizon;
        if (effect->type == SH_FX_SKID) {
            int size = muldiv(5000, cam->focus, depth);
            if (size < 1) size = 1;
            if (size > 6) size = 6;
            rect(fb, x - size / 2, y - 1, size, 2, 30);
        } else {
            SHSprite sprite;
            unsigned frame = (unsigned)effect->age * 6u / effect->life;
            uint16_t id;
            int w, h;
            if (frame > 5) frame = 5;
            id = effect->type == SH_FX_SPARK ? spark_frames[frame] :
                 smoke_frames[effect->variant % 3u][frame];
            sha_get_sprite(id, &sprite);
            w = muldiv((int32_t)sprite.world_width, cam->focus, depth);
            h = muldiv((int32_t)sprite.world_height, cam->focus, depth);
            if (w > 0 && h > 0 && w < 80 && h < 80)
                draw_sprite_scaled(fb, &sprite, x, y, w, h, 0, &cam->vp);
        }
    }
}

static void draw_walls(volatile uint8_t *fb, const Camera *cam)
{
    int32_t ca = sha_cos30(cam->angle), sa = sha_sin30(cam->angle);
    unsigned i;
    for (i = 0; i < cam->visible_wall_count; ++i) {
        const uint8_t *record = cam->assets.walls +
                                (uint32_t)cam->visible_walls[i] * 20u;
        SHWall wall; SHSprite tex;
        uint32_t wx0, wy0, wx1, wy1;
        int32_t x0, y0, x1, y1, rx0, rz0, rx1, rz1;
        int px0, px1, bottom0, bottom1, top0, top1, x;
        wall.x0 = sha_rd32(record); wall.y0 = sha_rd32(record + 4);
        wall.x1 = sha_rd32(record + 8); wall.y1 = sha_rd32(record + 12);
        wall.texture = sha_rd16(record + 16); wall.flags = sha_rd16(record + 18);
        sha_get_sprite(wall.texture, &tex);
        /* Importer already applied SEC_TOMAP; keep shifts/adds out of this loop. */
        wx0 = wall.x0; wy0 = wall.y0;
        wx1 = wall.x1; wy1 = wall.y1;
        x0 = (int32_t)(wx0 - cam->x) >> 6; y0 = (int32_t)(wy0 - cam->y) >> 6;
        x1 = (int32_t)(wx1 - cam->x) >> 6; y1 = (int32_t)(wy1 - cam->y) >> 6;
        rz0 = mul30(x0, ca) + mul30(y0, sa);
        rz1 = mul30(x1, ca) + mul30(y1, sa);
        rx0 = mul30(y0, ca) - mul30(x0, sa);
        rx1 = mul30(y1, ca) - mul30(x1, sa);
        if (rz0 < (8 << 12) && rz1 < (8 << 12)) continue;
        /* sectors.c limits traversal detail; this equivalent distance cull is
         * essential on 32X and avoids drawing the complete circuit each frame. */
        if (rz0 > (14 << 20) && rz1 > (14 << 20)) continue;
        if (rz0 < (8 << 12)) { int32_t t = muldiv((8 << 12) - rz0, 65536, rz1 - rz0); rx0 += muldiv(rx1-rx0,t,65536); rz0 = 8 << 12; }
        if (rz1 < (8 << 12)) { int32_t t = muldiv((8 << 12) - rz1, 65536, rz0 - rz1); rx1 += muldiv(rx0-rx1,t,65536); rz1 = 8 << 12; }
        px0 = cam->vp.cx + muldiv(rx0 >> 4, cam->focus, rz0);
        px1 = cam->vp.cx + muldiv(rx1 >> 4, cam->focus, rz1);
        if (px0 == px1 || (px0 < 0 && px1 < 0) || (px0 >= 320 && px1 >= 320)) continue;
        bottom0 = cam->vp.proj_y - cam->horizon + muldiv(cam->height << 2, cam->focus, rz0);
        bottom1 = cam->vp.proj_y - cam->horizon + muldiv(cam->height << 2, cam->focus, rz1);
        top0 = cam->vp.proj_y - cam->horizon + muldiv((cam->height << 2) - ((int)tex.height << 8), cam->focus, rz0);
        top1 = cam->vp.proj_y - cam->horizon + muldiv((cam->height << 2) - ((int)tex.height << 8), cam->focus, rz1);
        if (px0 > px1) { int t; t=px0;px0=px1;px1=t; t=bottom0;bottom0=bottom1;bottom1=t; t=top0;top0=top1;top1=t; }
        /* DDA all horizontal interpolation: no division remains in the X loop. */
        {
            int den = px1 - px0;
            int xstart = px0 < 0 ? 0 : px0;
            int xend = px1 >= 320 ? 319 : px1;
            int32_t top_step, bottom_step, tx_step;
            int32_t top_fp, bottom_fp, tx_fp;
            if (den <= 0 || xstart > xend) continue;
            top_step = muldiv(top1 - top0, 65536, den);
            bottom_step = muldiv(bottom1 - bottom0, 65536, den);
            tx_step = muldiv(tex.width, 65536, den);
            top_fp = (top0 << 16) + (xstart - px0) * top_step;
            bottom_fp = (bottom0 << 16) + (xstart - px0) * bottom_step;
            tx_fp = (xstart - px0) * tx_step;
            top_step <<= 1; bottom_step <<= 1; tx_step <<= 1;

            /* Low wall detail mirrors WallsDetail=0 and writes 2x2 quads. */
            for (x = xstart; x <= xend; x += 2,
                 top_fp += top_step, bottom_fp += bottom_step, tx_fp += tx_step) {
                int top = top_fp >> 16;
                int bottom = bottom_fp >> 16;
                int tx = tx_fp >> 16;
                int unclipped_top;
                int span;
                if (tx >= tex.width) tx -= tex.width;
                unclipped_top = top;
                span = bottom - top + 1;
                /* all values below are incremental or bit-shifted */
                uint32_t tyfp, tystep;
                int y;
                if (span <= 0) continue;
                tystep = (uint32_t)sh2_idiv((int32_t)tex.height << 16, span);
                if (top < cam->vp.y) top = cam->vp.y;
                if (bottom >= cam->vp.y + cam->vp.h) bottom = cam->vp.y + cam->vp.h - 1;
                tyfp = (uint32_t)(top - unclipped_top) * tystep;
                tystep <<= 1;
                for (y = top; y <= bottom; y += 2, tyfp += tystep) {
                    int ty = (int)(tyfp >> 16);
                    uint8_t c;
                    if (ty >= tex.height) ty = tex.height - 1;
                    c = tex.pixels[ty * tex.width + tx];
                    if (c) {
                        fb[y * 320 + x] = c;
                        if (x + 1 < 320) fb[y * 320 + x + 1] = c;
                        if (y + 1 <= bottom) {
                            fb[(y + 1) * 320 + x] = c;
                            if (x + 1 < 320) fb[(y + 1) * 320 + x + 1] = c;
                        }
                    }
                }
            }
        }
    }
}

static void draw_minimap(volatile uint8_t *fb, const Camera *cam, const SHCar *p)
{
    static uint8_t cache[64 * 64];
    static uint8_t age;
    static uint8_t cached_track = 255;
    int y, x;
    if (cached_track != cam->track) {
        cached_track = cam->track;
        age = 0;
    }
    if (age == 0) {
        int32_t ca = sha_cos30(p->angle), sa = sha_sin30(p->angle);
        uint32_t *clear = (uint32_t *)cache;
        for (x = 0; x < 64 * 64 / 4; ++x) clear[x] = 0;
        for (y = -32; y < 32; y += 2)
            for (x = -32; x < 32; x += 2) {
                uint32_t wx = p->x + (uint32_t)(mul30(x << 22, ca) - mul30(y << 22, sa));
                uint32_t wy = p->y + (uint32_t)(mul30(x << 22, sa) + mul30(y << 22, ca));
                uint8_t c = floor_pixel(cam->assets.map, cam->assets.tiles,
                                        cam->assets.cached_tiles,
                                        cam->assets.cached_tile_count, wx, wy);
                int dx = 32 + x, dy = 32 + y;
                cache[dy * 64 + dx] = cache[dy * 64 + dx + 1] = c;
                cache[(dy + 1) * 64 + dx] = cache[(dy + 1) * 64 + dx + 1] = c;
            }
    }
    age = (uint8_t)((age + 1) & 7);
    /* The renderer redraws the whole viewport every frame, so the minimap must
     * be copied to the framebuffer every frame even though its cache is only
     * recomputed every 8 frames. */
    for (y = 0; y < 64; ++y) {
        const uint32_t *src = (const uint32_t *)(cache + y * 64);
        volatile uint32_t *dst = (volatile uint32_t *)(fb + (cam->vp.y + 3 + y) * 320 + 252);
        for (x = 0; x < 16; ++x) dst[x] = src[x];
    }
    rect(fb, 282, cam->vp.y + 33, 5, 5, 14);
}

static void draw_number(volatile uint8_t *fb, unsigned value, uint16_t font0, int x, int y, int digits)
{
    unsigned divisor = 1; int i;
    for (i=1;i<digits;++i) divisor*=10;
    for (i=0;i<digits;++i) { unsigned d=(value/divisor)%10; draw_sprite_xy(fb,(uint16_t)(font0+d),x,y); { SHSprite sp; sha_get_sprite((uint16_t)(font0+d),&sp); x+=sp.width+1; } divisor/=10; }
}

static void draw_hud(volatile uint8_t *fb, const Camera *cam)
{
    const SHCar *p = cam->view_car;
    const int top = cam->vp.y;
    const int h = cam->vp.h;
    unsigned speed=(unsigned)abs32(muldiv(p->v,p->max_speed,1<<22));
    unsigned lap=p->nlap+1; if(lap>SH_LAPS)lap=SH_LAPS;
    if (cam->cockpit) {
        const uint8_t *cockpit = sha_ptr(p->car_type ? SHA_COCKPIT1_OFF : SHA_COCKPIT0_OFF);
        int bytes = p->car_type ? 17920 : 23360;
        int ch=bytes/320,y,x,base=top+h-ch;
        for(y=0;y<ch;++y)for(x=0;x<320;++x){uint8_t c=cockpit[y*320+x];if(c)fb[(base+y)*320+x]=c;}
    }
    if (h < 140) {
        /* Compact split-screen HUD: lap + speed in the top band only. */
        draw_sprite_xy(fb, SHSPR_MLAPS_IS2, cam->vp.cx-18, top+2);
        draw_number(fb,lap,SHSPR_MFBG0_IS2,cam->vp.cx-15,top+12,1);
        draw_sprite_xy(fb,SHSPR_MFBGB_IS2,cam->vp.cx+1,top+22);
        draw_number(fb,SH_LAPS,SHSPR_MFMG0_IS2,cam->vp.cx+10,top+23,1);
        if (!cam->cockpit) {
            draw_sprite_xy(fb, SHSPR_MPOS_IS2, 8, top+2);
            draw_number(fb,p->position,SHSPR_MFBW0_IS2,4,top+12,1);
            draw_number(fb,speed,SHSPR_MFMW0_IS2,12,top+30,3);
        }
        return;
    }
    draw_sprite_xy(fb, SHSPR_MLAPS_IS2, cam->vp.cx-18, top+2);
    draw_number(fb,lap,SHSPR_MFBG0_IS2,cam->vp.cx-15,top+12,1);
    draw_sprite_xy(fb,SHSPR_MFBGB_IS2,cam->vp.cx+1,top+22);
    draw_number(fb,SH_LAPS,SHSPR_MFMG0_IS2,cam->vp.cx+10,top+23,1);
    draw_sprite_xy(fb,SHSPR_MPOS_IS2,8,top+146);
    draw_number(fb,p->position,SHSPR_MFBW0_IS2,4,top+169,1);
    draw_sprite_xy(fb,SHSPR_MPOSBAR_IS2,22,top+182);
    draw_number(fb,SH_RACERS,SHSPR_MFMW0_IS2,31,top+181,1);
    if (!cam->cockpit) {
        draw_sprite_xy(fb,p->car_type ? SHSPR_MREVO1_IS2 : SHSPR_MREVO0_IS2,
                       228,top+126);
        draw_number(fb,speed,SHSPR_MFMW0_IS2,263,top+180,3);
    }
    draw_minimap(fb,cam,p);
}

static void render_race(volatile uint8_t *fb, const SHGame *game)
{
    Camera cam;
    uint8_t stamp = platform_profile_ticks();
    uint8_t next;
    render_profile.object_candidates = 0;
    make_camera(game, &game->player, game->player.sector, full_viewport(), &cam);
    build_visible_sectors(&cam);
    next = platform_profile_ticks();
    render_profile.visibility = (uint8_t)(next - stamp); stamp = next;
#ifdef ENABLE_DUAL_SH2_RENDER
    start_slave_floor(fb, &cam);
    draw_background(fb,&cam);
    next = platform_profile_ticks();
    render_profile.panorama = (uint8_t)(next - stamp); stamp = next;
    render_profile.floor_slave = wait_slave_floor();
    next = platform_profile_ticks();
    render_profile.floor_wait = (uint8_t)(next - stamp); stamp = next;
#else
    draw_background(fb,&cam);
    next = platform_profile_ticks();
    render_profile.panorama = (uint8_t)(next - stamp); stamp = next;
    draw_floor(fb,&cam);
    next = platform_profile_ticks();
    render_profile.floor_slave = (uint8_t)(next - stamp);
    render_profile.floor_wait = render_profile.floor_slave;
    stamp = next;
#endif
#ifndef SH_RENDER_FLOOR_ONLY
#ifndef SH_NO_WALLS
    draw_walls(fb,&cam);
#endif
    next = platform_profile_ticks();
    render_profile.walls = (uint8_t)(next - stamp); stamp = next;
#ifndef SH_NO_OBSTACLES
    render_profile.object_candidates = draw_obstacles(fb,&cam);
#endif
    next = platform_profile_ticks();
    render_profile.obstacles = (uint8_t)(next - stamp); stamp = next;
    draw_effects(fb, &cam, game);
#ifndef SH_NO_MODELS
    draw_race_cars(fb, &cam, game);
#endif
#endif
    next = platform_profile_ticks();
    render_profile.cars_effects = (uint8_t)(next - stamp); stamp = next;
#ifndef SH_NO_HUD
    if(game->hud)draw_hud(fb,&cam);
#endif
    if(game->mode==SH_MODE_COUNTDOWN){
        int sec=game->countdown/70; uint16_t id=SHSPR_RACE_0_IS2;
        if(sec==1)id=SHSPR_RACE_1_IS2;else if(sec==2)id=SHSPR_RACE_2_IS2;else if(sec>=3)id=SHSPR_RACE_3_IS2;
        draw_sprite_centered(fb,id,cam.vp.cx,cam.vp.cy);
    }
    if(game->mode==SH_MODE_PAUSED)draw_sprite_centered(fb,SHSPR_PAUSE_IS2,cam.vp.cx,cam.vp.cy-35);
    if(game->mode==SH_MODE_FINISHED)draw_sprite_centered(fb,game->player.position==1?SHSPR_YOUWIN_IS2:SHSPR_ENDRACE_IS2,cam.vp.cx,cam.vp.cy-35);
    next = platform_profile_ticks();
    render_profile.hud = (uint8_t)(next - stamp);
    /* Slave floor overlaps panorama; total is elapsed master critical path. */
    render_profile.total = render_profile.visibility + render_profile.panorama +
                           render_profile.floor_wait + render_profile.walls +
                           render_profile.obstacles + render_profile.cars_effects +
                           render_profile.hud;
    render_profile.sector_count = cam.visible_sector_count;
    render_profile.wall_count = cam.visible_wall_count;
}

/* Render one complete viewport entirely on the master SH-2. Used for split
 * screen, where the two viewports each need a full background+floor+world
 * pass and the shared slave cannot cheaply do both. */
static void render_view_master(volatile uint8_t *fb, const SHGame *game, Camera *cam)
{
    build_visible_sectors(cam);
    draw_background(fb, cam);
    draw_floor(fb, cam);
#ifndef SH_NO_WALLS
    draw_walls(fb, cam);
#endif
#ifndef SH_NO_OBSTACLES
    draw_obstacles(fb, cam);
#endif
    draw_effects(fb, cam, game);
#ifndef SH_NO_MODELS
    draw_race_cars(fb, cam, game);
#endif
    if (game->hud) draw_hud(fb, cam);
    if (game->mode == SH_MODE_COUNTDOWN) {
        int sec = game->countdown / 70;
        uint16_t id = SHSPR_RACE_0_IS2;
        if (sec == 1) id = SHSPR_RACE_1_IS2;
        else if (sec == 2) id = SHSPR_RACE_2_IS2;
        else if (sec >= 3) id = SHSPR_RACE_3_IS2;
        draw_sprite_centered(fb, id, cam->vp.cx, cam->vp.cy);
    }
    if (game->mode == SH_MODE_PAUSED)
        draw_sprite_centered(fb, SHSPR_PAUSE_IS2, cam->vp.cx, cam->vp.cy - 20);
    if (game->mode == SH_MODE_FINISHED)
        draw_sprite_centered(fb, game->player.position == 1 ? SHSPR_YOUWIN_IS2
                                                            : SHSPR_ENDRACE_IS2,
                             cam->vp.cx, cam->vp.cy - 20);
}

/* Two-player split screen: two stacked half-height viewports, both rendered
 * on the master SH-2. Player 1 on top, player 2 on the bottom. */
static void render_split(volatile uint8_t *fb, const SHGame *game)
{
    Camera cam;
    make_camera(game, &game->player, game->player.sector,
                split_viewport(0), &cam);
    render_view_master(fb, game, &cam);
    make_camera(game, &game->player2, game->player2.sector,
                split_viewport(1), &cam);
    render_view_master(fb, game, &cam);
}

static void draw_menu_background(volatile uint8_t *fb, uint32_t offset)
{
    const uint8_t *p = sha_ptr(offset);
    int y;
    fill(fb, 0);
    for (y = 0; y < 200; ++y) {
        const uint32_t *src = (const uint32_t *)(p + y * 320);
        volatile uint32_t *dst = (volatile uint32_t *)(fb + (VIEW_Y + y) * 320);
        int x;
        for (x = 0; x < 80; ++x) dst[x] = src[x];
    }
}

static void draw_menu_car_preview(volatile uint8_t *fb, const SHGame *game)
{
    SHSprite sprite;
    uint16_t view = (uint16_t)((game->frame >> 3) & 15u);
    sprite.pixels = sha_ptr(SHA_CAR_SPRITES_OFF) +
                    (((uint32_t)(game->car_type & 1u) * 6u +
                      (game->selected_car % 6u)) * 16u + view) * (64u * 48u);
    sprite.width = 64; sprite.height = 48; sprite.dx = 32; sprite.dy = 48;
    sprite.world_width = sprite.world_height = 0;
    { Viewport fv = full_viewport();
      draw_sprite_scaled(fb, &sprite, 160, VIEW_Y + 164, 128, 96, 0, &fv); }
}

static void draw_menu(volatile uint8_t *fb, const SHGame *game)
{
    static const char *track_names[2] = {"RACERS EDGE", "THE CITY"};
    static const char *class_names[2] = {"FORMULA ONE", "STOCK CARS"};
    static const char *car_names[2][6] = {
        {"PHOENIX ENGINE", "BLUE STEEL", "DREAM MAKER", "FROZEN SKY", "SPEED DEMON", "BLACK BULLET"},
        {"ROAD STAR", "THE FLAME", "SKEIN", "BLACK FURY", "LUCKY HORSE", "THE MIRACLE"}
    };
    if (game->menu_page == SH_MENU_MAIN) {
        draw_menu_background(fb, SHA_MENU_MAIN_OFF);
        rect(fb,55,VIEW_Y+55,210,77,160); rect(fb,58,VIEW_Y+58,204,71,31);
        text(fb, game->split ? 94 : 106, VIEW_Y + 67,
             game->split ? "TWO PLAYERS" : "ONE PLAYER", 96);
        {
            const char *track = track_names[game->selected_track & 1u];
            const char *class_name = class_names[game->car_type & 1u];
            text(fb,160-text_width(track)/2,VIEW_Y+84,track,96);
            text(fb,160-text_width(class_name)/2,VIEW_Y+101,class_name,96);
        }
        text(fb,145,VIEW_Y+145,"GO",96);
    } else if (game->menu_page == SH_MENU_CIRCUIT) {
        draw_menu_background(fb, SHA_MENU_CIRCUIT_OFF);
        rect(fb, 74, VIEW_Y + 62, 172, 45, 160);
        rect(fb, 77, VIEW_Y + 65, 166, 39, 31);
        text(fb, 112, VIEW_Y + 70, "CIRCUIT", 96);
        text(fb, game->selected_track ? 136 : 121, VIEW_Y + 88,
             track_names[game->selected_track & 1u], 96);
        text(fb, 73, VIEW_Y + 180, "LEFT RIGHT  START", 96);
    } else if (game->menu_page == SH_MENU_CLASS) {
        draw_menu_background(fb, SHA_MENU_CAR_OFF);
        rect(fb, 74, VIEW_Y + 50, 172, 45, 160);
        rect(fb, 77, VIEW_Y + 53, 166, 39, 31);
        text(fb, 115, VIEW_Y + 58, "CAR CLASS", 96);
        text(fb, game->car_type ? 127 : 124, VIEW_Y + 76,
             class_names[game->car_type & 1u], 96);
        text(fb, 73, VIEW_Y + 180, "LEFT RIGHT  START", 96);
    } else {
        draw_menu_background(fb, SHA_MENU_CAR_OFF);
        draw_menu_car_preview(fb, game);
        rect(fb, 55, VIEW_Y + 28, 210, 31, 160);
        rect(fb, 58, VIEW_Y + 31, 204, 25, 31);
        text(fb, 108, VIEW_Y + 36, "SELECT CAR", 96);
        {
            const char *name = car_names[game->car_type & 1u][game->selected_car % 6u];
            text(fb, 160 - text_width(name) / 2, VIEW_Y + 48, name, 96);
        }
        text(fb, 73, VIEW_Y + 180, "LEFT RIGHT  START", 96);
    }
}

static void write_profile_word(volatile uint8_t *fb, int x, int y, uint16_t value)
{
    fb[y * 320 + x + 0] = (uint8_t)(224u + ((value >> 12) & 15u));
    fb[y * 320 + x + 1] = (uint8_t)(224u + ((value >> 8) & 15u));
    fb[y * 320 + x + 2] = (uint8_t)(224u + ((value >> 4) & 15u));
    fb[y * 320 + x + 3] = (uint8_t)(224u + (value & 15u));
}

static void write_profile_probes(volatile uint8_t *fb)
{
    unsigned i;
    const uint16_t phases[9] = {
        render_profile.visibility, render_profile.panorama,
        render_profile.floor_slave, render_profile.floor_wait,
        render_profile.walls, render_profile.obstacles,
        render_profile.cars_effects, render_profile.hud, render_profile.total
    };
    for (i = 0; i < 16; ++i) fb[223 * 320 + 288 + i] = (uint8_t)(224u + i);
    for (i = 0; i < 9; ++i) write_profile_word(fb, 272 + (int)i * 4, 222, phases[i]);
    write_profile_word(fb, 296, 221, render_profile.sector_count);
    write_profile_word(fb, 300, 221, render_profile.wall_count);
    write_profile_word(fb, 304, 221, render_profile.object_candidates);
}

void sh_render_frame(volatile uint8_t *fb, const SHGame *game)
{
    static uint8_t title_palette=255;
    if(game->mode==SH_MODE_TITLE){
        if(title_palette!=1){platform_set_vga_palette(sha_ptr(SHA_TITLE_PALETTE_OFF));title_palette=1;}
        fill(fb,0); {const uint8_t *p=sha_ptr(SHA_TITLE_OFF);int y,x;for(y=0;y<200;++y)for(x=0;x<320;++x)fb[(VIEW_Y+y)*320+x]=p[y*320+x];}
        if(game->frame&32)text(fb,124,VIEW_Y+188,"PRESS START",255);
    }else if(game->mode==SH_MODE_MENU){
        if(title_palette!=0){platform_set_vga_palette(sha_ptr(SHA_GAME_PALETTE_OFF));title_palette=0;}
        draw_menu(fb, game);
    }else{
        if(title_palette!=0){platform_set_vga_palette(sha_ptr(SHA_GAME_PALETTE_OFF));title_palette=0;}
        /* The race renderer overwrites the complete 320x200 viewport. Only
         * clear the overscan bands instead of erasing and redrawing 71K bytes. */
        if (game->split) {
            fill(fb, 0);
            render_split(fb, game);
        } else {
            rect(fb, 0, 0, 320, VIEW_Y, 0);
            rect(fb, 0, VIEW_Y + VIEW_H, 320, 224 - VIEW_Y - VIEW_H, 0);
            render_race(fb,game);
        }
    }
    /* Overscan probes expose render phases/counts plus collision, movement,
     * camera, state and heartbeat to the PicoDrive point-to-point test. */
    write_profile_probes(fb);
    write_profile_word(fb, 308, 221, game->qa_route_point);
    write_profile_word(fb, 308, 220, game->qa_collision_point);
    fb[223 * 320 + 311] = game->qa_route ? 96 : 0;
    fb[223 * 320 + 312] = game->qa_route == 2 ? 96 : 0;
    fb[223 * 320 + 313] = game->wall_collision_count ? 96 : 0;
    fb[223 * 320 + 314] = (uint8_t)(224 + ((game->player.x >> 20) & 15u));
    fb[223 * 320 + 315] = (uint8_t)(224 + ((game->player.y >> 20) & 15u));
    fb[223 * 320 + 316] = game->collision_count ? 96 : 0;
    fb[223 * 320 + 309] = game->car_collision_count ? 96 : 0;
    fb[223 * 320 + 317] =
        abs32((int16_t)(game->player.movangle - game->camera_angle)) >= 32 ? 96 : 0;
    fb[223 * 320 + 318] = (uint8_t)(game->mode == SH_MODE_MENU ?
                                    24 + game->menu_page : 16 + game->mode);
    fb[223 * 320 + 319] = (uint8_t)(224 + (game->frame & 15));
}
