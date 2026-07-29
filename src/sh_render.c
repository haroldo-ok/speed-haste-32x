/*
 * Speed Haste software renderer for the 32X framebuffer.
 * Floor/camera formulas follow game/3dfloor.c and game/fla.asm; object and
 * camera transforms follow game/flsprs.c and game/race.c.
 */
#include "sh_render.h"
#include "sh_assets.h"
#include "platform/platform.h"
#include "platform/mars.h"
#include "sh_render_worker.h"

#define VIEW_Y 12
#define VIEW_H 200
#define CX 160
#define CY (VIEW_Y + 100)
#define MAX_VISIBLE 16
#define PLAYER_MAX_SPEED 310

static int32_t abs32(int32_t v) { return v < 0 ? -v : v; }
static int32_t mul30(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 30); }
static int32_t muldiv(int32_t a, int32_t b, int32_t c) { return c ? (int32_t)(((int64_t)a * b) / c) : 0; }

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

static inline __attribute__((always_inline)) uint8_t floor_pixel(uint32_t x, uint32_t y)
{
    const uint8_t *map = sha_ptr(SHA_MAP128_OFF);
    const uint8_t *tiles = sha_ptr(SHA_TILES_OFF);
    uint32_t gx = x >> 25, gy = y >> 25;
    uint8_t tile;
    if (gx >= 128 || gy >= 128) return 0;
    tile = map[gy * 128u + gx];
    return tiles[(uint32_t)tile * 4096u + (((y >> 19) & 63u) << 6) + ((x >> 19) & 63u)];
}

typedef struct Camera {
    uint32_t x, y, target_x, target_y;
    uint16_t angle;
    int32_t radius;
    int32_t height, focus, horizon;
    uint8_t cockpit;
} Camera;

static void make_camera(const SHGame *game, Camera *cam)
{
    const SHCar *p = &game->player;
    cam->target_x = p->x;
    cam->target_y = p->y;
    cam->angle = game->camera == 1 ? p->angle : p->movangle;
    cam->cockpit = 0;
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

static void draw_background(volatile uint8_t *fb, const Camera *cam)
{
    const uint8_t *sky = sha_ptr(SHA_SKY_OFF);
    const uint8_t *mountains = sha_ptr(SHA_MOUNTAINS_OFF);
    const int sky_h = 70, mountain_h = 49;
    int offset = ((uint32_t)3 * cam->angle * 320u / 65536u) % 320;
    int y, x;
    for (y = 0; y < sky_h; ++y) {
        const uint8_t *src = sky + y * 320;
        volatile uint16_t *dst = (volatile uint16_t *)(fb + (VIEW_Y + y) * 320);
        for (x = 0; x < 320; x += 2) {
            int sx0 = x + 320 - offset;
            int sx1;
            if (sx0 >= 320) sx0 -= 320;
            sx1 = sx0 + 1; if (sx1 == 320) sx1 = 0;
            dst[x >> 1] = (uint16_t)((src[sx0] << 8) | src[sx1]);
        }
    }
    for (y = 0; y < mountain_h; ++y) {
        const uint8_t *src = mountains + y * 320;
        int yy = VIEW_Y + sky_h - mountain_h + y;
        for (x = 0; x < 320; ++x) {
            uint8_t c = src[(x + 320 - offset) % 320];
            if (c) fb[yy * 320 + x] = c;
        }
    }
}

static void draw_floor(volatile uint8_t *fb, const Camera *cam)
{
    const uint8_t *translation = sha_ptr(SHA_COLOR_MAP_OFF);
    const int floor_y = VIEW_Y + 70;
    int row;
    /* The 32X cartridge bus is shared by both SH-2s. Render the floor at
     * 80x65 samples and expand 4x2, matching the chunky low-detail option of
     * the DOS game while reducing ROM tile fetches by eight. */
    for (row = 0; row < VIEW_H - 70; row += 2) {
        int Y = cam->horizon + row;
        int32_t radius = muldiv(cam->height, cam->focus, Y);
        int level;
        int32_t step_x, step_y, pos_x, pos_y;
        int x;
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
        pos_x += step_x + step_x;
        pos_y += step_y + step_y;
        step_x <<= 2;
        step_y <<= 2;
        for (x = 0; x < 320; x += 4) {
            uint8_t color = floor_pixel((uint32_t)pos_x, (uint32_t)pos_y);
            uint8_t shaded = translation[(31 - level) * 256 + color];
            volatile uint32_t *p0 = (volatile uint32_t *)(fb + (floor_y + row) * 320 + x);
            volatile uint32_t *p1 = (volatile uint32_t *)((volatile uint8_t *)p0 + 320);
            uint32_t pattern = (uint32_t)shaded * 0x01010101u;
            *p0 = pattern;
            if (floor_y + row + 1 < VIEW_Y + VIEW_H)
                *p1 = pattern;
            pos_x += step_x;
            pos_y += step_y;
        }
    }
}

void sh_render_slave_floor(const volatile SHFloorJob *job)
{
    Camera cam;
    cam.x = job->camera_x;
    cam.y = job->camera_y;
    cam.angle = job->angle;
    cam.height = job->height;
    cam.focus = job->focus;
    cam.horizon = job->horizon;
    cam.target_x = cam.target_y = 0;
    cam.radius = 0;
    cam.cockpit = 0;
    draw_floor(job->framebuffer, &cam);
}

#ifdef ENABLE_DUAL_SH2_RENDER
static void start_slave_floor(volatile uint8_t *fb, const Camera *cam)
{
    volatile SHFloorJob *job = SH_FLOOR_JOB;
    while (MARS_SYS_COMM6 != SH_SLAVE_IDLE) { }
    job->framebuffer = fb;
    job->camera_x = cam->x;
    job->camera_y = cam->y;
    job->angle = cam->angle;
    job->height = cam->height;
    job->focus = cam->focus;
    job->horizon = cam->horizon;
    MARS_SYS_COMM6 = SH_SLAVE_DRAW_FLOOR;
}

static void wait_slave_floor(void)
{
    while (MARS_SYS_COMM6 != SH_SLAVE_FLOOR_DONE) { }
    MARS_SYS_COMM6 = SH_SLAVE_IDLE;
}
#endif

static void draw_sprite_scaled(volatile uint8_t *fb, const SHSprite *sp,
                               int center_x, int base_y, int width, int height, int mirror)
{
    int y, x;
    uint32_t syfp = 0, systep, sxstep;
    if (!width || !height) return;
    if (width < 0) { width = -width; mirror ^= 1; }
    if (width > 240 || height > 200) return;
    systep = ((uint32_t)sp->height << 16) / (uint32_t)height;
    sxstep = ((uint32_t)sp->width << 16) / (uint32_t)width;
    systep <<= 1;
    sxstep <<= 1;
    for (y = 0; y < height; y += 2, syfp += systep) {
        int sy = (int)(syfp >> 16);
        int dy = base_y - height + y;
        uint32_t sxfp = 0;
        if (dy < VIEW_Y || dy >= VIEW_Y + VIEW_H) continue;
        for (x = 0; x < width; x += 2, sxfp += sxstep) {
            int sx = (int)(sxfp >> 16);
            int dx = center_x - width / 2 + x;
            uint8_t c;
            if (mirror) sx = sp->width - 1 - sx;
            if (dx < 0 || dx >= 320) continue;
            c = sp->pixels[sy * sp->width + sx];
            if (c) {
                fb[dy * 320 + dx] = c;
                if (dx + 1 < 320) fb[dy * 320 + dx + 1] = c;
                if (dy + 1 < VIEW_Y + VIEW_H) {
                    fb[(dy + 1) * 320 + dx] = c;
                    if (dx + 1 < 320) fb[(dy + 1) * 320 + dx + 1] = c;
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
    draw_sprite_xy(fb, id, cx - sp.width / 2 + sp.dx, cy - sp.height / 2 + sp.dy);
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
    sha_get_model(car->model % 6, &model);
    for (i = 0; i < model.vertex_count; ++i) {
        int32_t x = sha_rds32(model.data + i * 12u);
        int32_t y = sha_rds32(model.data + i * 12u + 4);
        int32_t z = sha_rds32(model.data + i * 12u + 8);
        int32_t dz = (depth >> 4) + mul30(z, ca) - mul30(x, sa);
        vx[i] = ((side >> 4) + mul30(x, ca) + mul30(z, sa)) >> 6;
        vy[i] = (((cam->height << 2) - ((int32_t)car->z >> 10) - y) >> 6);
        vz[i] = dz >> 2;
        if (vz[i] < 64) vz[i] = 64;
        sx[i] = (int16_t)(CX + muldiv(vx[i], cam->focus, vz[i]));
        sy[i] = (int16_t)(CY - cam->horizon + muldiv(vy[i], cam->focus, vz[i]));
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
    x = CX + muldiv(side >> 4, cam->focus, depth);
    y = CY + muldiv(cam->height << 2, cam->focus, depth) - cam->horizon;
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
    x = CX + muldiv(side >> 4, cam->focus, depth);
    y = CY + muldiv((cam->height << 2) - ((int32_t)car->z >> 10), cam->focus, depth) - cam->horizon;
    w = muldiv(9000, cam->focus, depth); if (w < 2) w = 2;
    h = w * 3 / 4;
    view = (uint16_t)(((uint16_t)(car->angle - cam->angle + 2048u) >> 12) & 15u);
    sprite.pixels = sha_ptr(SHA_CAR_SPRITES_OFF) +
                    ((uint32_t)(car->model % 6) * 16u + view) * (64u * 48u);
    sprite.width = 64; sprite.height = 48; sprite.dx = sprite.dy = 0;
    sprite.world_width = sprite.world_height = 0;
    draw_sprite_scaled(fb, &sprite, x, y, w, h, 0);
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
}

typedef struct VisibleSprite { int32_t depth; int x, y, w, h; uint16_t id; } VisibleSprite;

static void draw_obstacles(volatile uint8_t *fb, const Camera *cam)
{
    VisibleSprite visible[MAX_VISIBLE];
    int count = 0;
    int32_t ca = sha_cos30(cam->angle), sa = sha_sin30(cam->angle);
    unsigned i;
    for (i = 0; i < SHA_OBSTACLE_COUNT; ++i) {
        SHObstacle ob; SHSprite sp;
        int32_t relx, rely, depth, side;
        int x, y, w, h;
        sha_get_obstacle(i, &ob);
        relx = (int32_t)(ob.x - cam->x) >> 6;
        rely = (int32_t)(ob.y - cam->y) >> 6;
        /* Reject most of the 263 decorations before any fixed-point multiplies. */
        if (abs32(relx) > (24 << 20) || abs32(rely) > (24 << 20)) continue;
        depth = mul30(relx, ca) + mul30(rely, sa);
        if (depth < (4 << 14) || depth > (24 << 20)) continue;
        side = mul30(rely, ca) - mul30(relx, sa);
        if (abs32(side / 3) > depth) continue;
        sha_get_sprite(ob.sprite, &sp);
        x = CX + muldiv(side >> 4, cam->focus, depth);
        y = CY + muldiv(cam->height << 2, cam->focus, depth) - cam->horizon;
        w = muldiv((int32_t)sp.world_width, cam->focus, depth);
        h = muldiv((int32_t)sp.world_height, cam->focus, depth);
        if (w <= 0 || h <= 0 || w > 200 || h > 200) continue;
        if (count < MAX_VISIBLE) {
            int k = count++;
            visible[k].depth = depth; visible[k].x = x; visible[k].y = y;
            visible[k].w = w; visible[k].h = h; visible[k].id = ob.sprite;
        }
    }
    for (i = 1; i < (unsigned)count; ++i) {
        VisibleSprite v = visible[i]; int j = i;
        while (j && visible[j-1].depth < v.depth) { visible[j] = visible[j-1]; --j; }
        visible[j] = v;
    }
    for (i = 0; i < (unsigned)count; ++i) {
        SHSprite sp; sha_get_sprite(visible[i].id, &sp);
        draw_sprite_scaled(fb, &sp, visible[i].x, visible[i].y, visible[i].w, visible[i].h, 0);
    }
}

static void draw_walls(volatile uint8_t *fb, const Camera *cam)
{
    int32_t ca = sha_cos30(cam->angle), sa = sha_sin30(cam->angle);
    unsigned i;
    for (i = 0; i < SHA_WALL_COUNT; ++i) {
        SHWall wall; SHSprite tex;
        uint32_t wx0, wy0, wx1, wy1;
        int32_t x0, y0, x1, y1, rx0, rz0, rx1, rz1;
        int px0, px1, bottom0, bottom1, top0, top1, x;
        sha_get_wall(i, &wall); sha_get_sprite(wall.texture, &tex);
        wx0 = (wall.x0 << 15) + (1u << 30); wy0 = (wall.y0 << 15) + (1u << 30);
        wx1 = (wall.x1 << 15) + (1u << 30); wy1 = (wall.y1 << 15) + (1u << 30);
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
        px0 = CX + muldiv(rx0 >> 4, cam->focus, rz0);
        px1 = CX + muldiv(rx1 >> 4, cam->focus, rz1);
        if (px0 == px1 || (px0 < 0 && px1 < 0) || (px0 >= 320 && px1 >= 320)) continue;
        bottom0 = CY - cam->horizon + muldiv(cam->height << 2, cam->focus, rz0);
        bottom1 = CY - cam->horizon + muldiv(cam->height << 2, cam->focus, rz1);
        top0 = CY - cam->horizon + muldiv((cam->height << 2) - ((int)tex.height << 8), cam->focus, rz0);
        top1 = CY - cam->horizon + muldiv((cam->height << 2) - ((int)tex.height << 8), cam->focus, rz1);
        if (px0 > px1) { int t; t=px0;px0=px1;px1=t; t=bottom0;bottom0=bottom1;bottom1=t; t=top0;top0=top1;top1=t; }
        /* Low wall detail mirrors the DOS WallsDetail=0 performance mode. */
        for (x = px0 < 0 ? 0 : px0; x <= px1 && x < 320; x += 2) {
            int den = px1 - px0; int t = den ? (x - px0) * 65536 / den : 0;
            int top = top0 + (top1-top0) * t / 65536;
            int bottom = bottom0 + (bottom1-bottom0) * t / 65536;
            int tx = ((x - px0) * tex.width / (den ? den : 1)) % tex.width;
            int unclipped_top = top, span = bottom - top + 1;
            uint32_t tyfp, tystep;
            int y;
            if (span <= 0) continue;
            tystep = ((uint32_t)tex.height << 16) / (uint32_t)span;
            if (top < VIEW_Y) top = VIEW_Y;
            if (bottom >= VIEW_Y + VIEW_H) bottom = VIEW_Y + VIEW_H - 1;
            tyfp = (uint32_t)(top - unclipped_top) * tystep;
            for (y = top; y <= bottom; ++y, tyfp += tystep) {
                int ty = (int)(tyfp >> 16);
                uint8_t c;
                if (ty >= tex.height) ty = tex.height - 1;
                c = tex.pixels[ty * tex.width + tx];
                if (c) {
                    fb[y * 320 + x] = c;
                    if (x + 1 < 320) fb[y * 320 + x + 1] = c;
                }
            }
        }
    }
}

static void draw_minimap(volatile uint8_t *fb, const SHCar *p)
{
    static uint8_t cache[64 * 64];
    static uint8_t age;
    int y, x;
    if (age == 0) {
        int32_t ca = sha_cos30(p->angle), sa = sha_sin30(p->angle);
        uint32_t *clear = (uint32_t *)cache;
        for (x = 0; x < 64 * 64 / 4; ++x) clear[x] = 0;
        for (y = -32; y < 32; y += 2)
            for (x = -32; x < 32; x += 2) {
                uint32_t wx = p->x + (uint32_t)(mul30(x << 22, ca) - mul30(y << 22, sa));
                uint32_t wy = p->y + (uint32_t)(mul30(x << 22, sa) + mul30(y << 22, ca));
                uint8_t c = floor_pixel(wx, wy);
                int dx = 32 + x, dy = 32 + y;
                cache[dy * 64 + dx] = cache[dy * 64 + dx + 1] = c;
                cache[(dy + 1) * 64 + dx] = cache[(dy + 1) * 64 + dx + 1] = c;
            }
    }
    age = (uint8_t)((age + 1) & 7);
    for (y = 0; y < 64; ++y) {
        const uint32_t *src = (const uint32_t *)(cache + y * 64);
        volatile uint32_t *dst = (volatile uint32_t *)(fb + (VIEW_Y + 3 + y) * 320 + 252);
        for (x = 0; x < 16; ++x) dst[x] = src[x];
    }
    rect(fb, 282, VIEW_Y + 33, 5, 5, 14);
}

static void draw_number(volatile uint8_t *fb, unsigned value, uint16_t font0, int x, int y, int digits)
{
    unsigned divisor = 1; int i;
    for (i=1;i<digits;++i) divisor*=10;
    for (i=0;i<digits;++i) { unsigned d=(value/divisor)%10; draw_sprite_xy(fb,(uint16_t)(font0+d),x,y); { SHSprite sp; sha_get_sprite((uint16_t)(font0+d),&sp); x+=sp.width+1; } divisor/=10; }
}

static void draw_hud(volatile uint8_t *fb, const SHGame *game, const Camera *cam)
{
    const SHCar *p=&game->player;
    unsigned speed=(unsigned)abs32(muldiv(p->v,PLAYER_MAX_SPEED,1<<22));
    unsigned lap=p->nlap+1; if(lap>SH_LAPS)lap=SH_LAPS;
    if (cam->cockpit) {
        const uint8_t *cockpit=sha_ptr(SHA_COCKPIT_OFF); int h=23360/320,y,x,base=VIEW_Y+VIEW_H-h;
        for(y=0;y<h;++y)for(x=0;x<320;++x){uint8_t c=cockpit[y*320+x];if(c)fb[(base+y)*320+x]=c;}
    }
    draw_sprite_xy(fb, SHSPR_MLAPS_IS2, CX-18, VIEW_Y+2);
    draw_number(fb,lap,SHSPR_MFBG0_IS2,CX-15,VIEW_Y+12,1);
    draw_sprite_xy(fb,SHSPR_MFBGB_IS2,CX+1,VIEW_Y+22);
    draw_number(fb,SH_LAPS,SHSPR_MFMG0_IS2,CX+10,VIEW_Y+23,1);
    draw_sprite_xy(fb,SHSPR_MPOS_IS2,8,VIEW_Y+146);
    draw_number(fb,p->position,SHSPR_MFBW0_IS2,4,VIEW_Y+169,1);
    draw_sprite_xy(fb,SHSPR_MPOSBAR_IS2,22,VIEW_Y+182);
    draw_number(fb,SH_RACERS,SHSPR_MFMW0_IS2,31,VIEW_Y+181,1);
    if (!cam->cockpit) {
        draw_sprite_xy(fb,SHSPR_MREVO0_IS2,228,VIEW_Y+126);
        draw_number(fb,speed,SHSPR_MFMW0_IS2,263,VIEW_Y+180,3);
    }
    draw_minimap(fb,p);
}

static void render_race(volatile uint8_t *fb, const SHGame *game)
{
    Camera cam;
    make_camera(game,&cam);
#ifdef ENABLE_DUAL_SH2_RENDER
    start_slave_floor(fb, &cam);
    draw_background(fb,&cam);
    wait_slave_floor();
#else
    draw_background(fb,&cam);
    draw_floor(fb,&cam);
#endif
#ifndef SH_RENDER_FLOOR_ONLY
#ifndef SH_NO_WALLS
    draw_walls(fb,&cam);
#endif
#ifndef SH_NO_OBSTACLES
    draw_obstacles(fb,&cam);
#endif
#ifndef SH_NO_MODELS
    draw_race_cars(fb, &cam, game);
#endif
#endif
#ifndef SH_NO_HUD
    if(game->hud)draw_hud(fb,game,&cam);
#endif
    if(game->mode==SH_MODE_COUNTDOWN){
        int sec=game->countdown/70; uint16_t id=SHSPR_RACE_0_IS2;
        if(sec==1)id=SHSPR_RACE_1_IS2;else if(sec==2)id=SHSPR_RACE_2_IS2;else if(sec>=3)id=SHSPR_RACE_3_IS2;
        draw_sprite_centered(fb,id,CX,CY);
    }
    if(game->mode==SH_MODE_PAUSED)draw_sprite_centered(fb,SHSPR_PAUSE_IS2,CX,CY-35);
    if(game->mode==SH_MODE_FINISHED)draw_sprite_centered(fb,game->player.position==1?SHSPR_YOUWIN_IS2:SHSPR_ENDRACE_IS2,CX,CY-35);
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
        fill(fb,0); {const uint8_t *p=sha_ptr(SHA_MENU_OFF);int y,x;for(y=0;y<200;++y)for(x=0;x<320;++x)fb[(VIEW_Y+y)*320+x]=p[y*320+x];}
        rect(fb,55,VIEW_Y+55,210,77,185);rect(fb,58,VIEW_Y+58,204,71,0);
        text(fb,106,VIEW_Y+67,"ONE PLAYER",185); text(fb,103,VIEW_Y+84,"RACERS EDGE",185);
        text(fb,103,VIEW_Y+101,"FORMULA ONE",185);text(fb,115,VIEW_Y+145,"START",185);
    }else{
        if(title_palette!=0){platform_set_vga_palette(sha_ptr(SHA_GAME_PALETTE_OFF));title_palette=0;}
        /* The race renderer overwrites the complete 320x200 viewport. Only
         * clear the overscan bands instead of erasing and redrawing 71K bytes. */
        rect(fb, 0, 0, 320, VIEW_Y, 0);
        rect(fb, 0, VIEW_Y + VIEW_H, 320, 224 - VIEW_Y - VIEW_H, 0);
        render_race(fb,game);
    }
    /* Two overscan pixels expose state/heartbeat to the headless test rig. */
    fb[223 * 320 + 318] = (uint8_t)(16 + game->mode);
    fb[223 * 320 + 319] = (uint8_t)(224 + (game->frame & 15));
}
