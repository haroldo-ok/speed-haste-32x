/*
 * Speed Haste 32X race simulation.
 * Derived from game/userctl.c, game/cars.c and game/racemap.c in the
 * original 1995 source release by Javier Arevalo Baeza.
 */
#include "sh_game.h"
#include "sh_assets.h"
#include "platform/sh2_math.h"

#define FP30_ONE (1L << 30)
#define MAX_REVO (1L << 22)
#define PLAYER_FRONT 0xB00000
#define PLAYER_REAR (-0x800000)
#define PLAYER_HALF_WIDTH 0x380000
#define WALL_RELEASE_MARGIN 4

static const uint16_t car_max_speed[2][6] = {
    {310, 315, 320, 320, 325, 330}, /* Formula One, CARS.LST */
    {240, 245, 250, 250, 255, 260}  /* Stock */
};

static uint16_t class_reference_speed(uint8_t car_type)
{
    return car_type ? 260 : 330;
}

static int32_t abs32(int32_t value) { return value < 0 ? -value : value; }
static int32_t mul30(int32_t value, int32_t trig) { return (int32_t)(((int64_t)value * trig) >> 30); }
static inline __attribute__((always_inline)) int32_t muldiv(int32_t a, int32_t b, int32_t c)
{
    return c ? sh2_muldiv(a, b, c) : 0;
}

/* Restoring square root: no divide and only used after the broad phase has
 * found an actual contact. Keeping it off the normal 70 Hz path is cheaper
 * than normalising every source wall in the scan. */
static uint32_t isqrt32(uint32_t value)
{
    uint32_t root = 0;
    uint32_t bit = 1u << 30;
    while (bit > value) bit >>= 2;
    while (bit) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

static void nearest_wall_point(int32_t px, int32_t py, int32_t x0, int32_t y0,
                               int32_t vx, int32_t vy, int32_t length2,
                               int32_t *qx, int32_t *qy)
{
    int32_t t = muldiv((px - x0) * vx + (py - y0) * vy, 65536, length2);
    if (t < 0) t = 0;
    else if (t > 65536) t = 65536;
    *qx = x0 + ((vx * t) >> 16);
    *qy = y0 + ((vy * t) >> 16);
}

/* Place the centre on the known road side of a wall with a small release
 * margin. The margin is deliberate hysteresis: a car that has just bounced or
 * is reversing away must not be reflected back into the same wall on the next
 * simulation tick. Coordinates here use the collision pass's 14.18 -> integer
 * reduction; converting through uint32_t keeps world wrapping defined. */
static void release_from_wall(SHCar *car, int32_t qx, int32_t qy,
                              int32_t nx, int32_t ny, int32_t radius)
{
    uint32_t distance = isqrt32((uint32_t)(nx * nx + ny * ny));
    int32_t rx, ry;
    if (!distance) return;
    rx = qx + muldiv(nx, radius, (int32_t)distance);
    ry = qy + muldiv(ny, radius, (int32_t)distance);
    car->x = ((uint32_t)rx) << 18;
    car->y = ((uint32_t)ry) << 18;
}

/* Original GearInfo table: ratio 16.16, acceleration and RPM limits. */
typedef struct GearInfo { int32_t ratio, accel, minrevo, maxrevo; } GearInfo;
static const GearInfo gears[] = {
    {-0x4000, 0x8000, 0, 1 << 28},
    { 0x0000, 0x20000, 0, 1 << 15},
    { 0x2000, 0x10000, 0, 0x0D << 18},
    { 0x4000, 0x07000, 4 << 18, 0x0D << 18},
    { 0x6000, 0x04000, 5 << 18, 0x0E << 18},
    { 0x8000, 0x02800, 5 << 18, 0x0D << 18},
    { 0xC000, 0x01C00, 5 << 18, 0x0D << 18},
    {0x10000, 0x01000, 6 << 18, 0x0D << 18}
};

static void get_start(uint8_t track, unsigned index, SHCar *car)
{
    SHTrackAssets assets;
    const uint8_t *p;
    sha_get_track(track, &assets);
    p = assets.starts + (index % assets.start_count) * 8u;
    uint32_t sx = sha_rd16(p), sy = sha_rd16(p + 2);
    car->x = (sx << 19) + (1u << 30);
    car->y = (sy << 19) + (1u << 30);
    car->z = 0x40000;
    car->angle = sha_rd16(p + 4);
    car->movangle = car->angle;
    car->ma = car->angle;
    car->sector = (int8_t)sha_find_sector(&assets, -1, car->x, car->y);
}

static void clear_car(SHCar *car)
{
    unsigned i;
    uint8_t *p = (uint8_t *)car;
    for (i = 0; i < sizeof(*car); ++i) p[i] = 0;
    car->gear = 1;
    car->automatic = 1;
    car->position = SH_RACERS;
}

uint8_t sh_ground_color(uint8_t track, uint32_t x, uint32_t y)
{
    SHTrackAssets assets;
    uint32_t gx = x >> 25, gy = y >> 25;
    uint8_t tile;
    sha_get_track(track, &assets);
    if (gx >= 128 || gy >= 128) return 0;
    tile = assets.map[gy * 128u + gx];
    {
        const uint8_t *pixels = tile < assets.cached_tile_count ?
            assets.cached_tiles + (uint32_t)tile * 4096u :
            assets.tiles + (uint32_t)tile * 4096u;
        return pixels[(((y >> 19) & 63u) << 6) + ((x >> 19) & 63u)];
    }
}

static void set_ai_target(uint8_t track, SHCar *car)
{
    SHPathPoint point;
    sha_get_path(track, car->next_point, &point);
    car->target_x = (point.x >> 1) + (1u << 30);
    car->target_y = (point.y >> 1) + (1u << 30);
}

static void reset_race(SHGame *game)
{
    unsigned i;
    /* Load the frequency-sorted map working set into aligned SDRAM. */
    sha_prepare_track(game->selected_track);
    clear_car(&game->player);
    get_start(game->selected_track, 0, &game->player);
    game->player.model = game->selected_car % 6;
    game->player.car_type = game->car_type & 1u;
    game->player.max_speed = car_max_speed[game->player.car_type][game->player.model];
    game->camera_angle = game->player.angle;

    /* Second player (split screen): own start slot and a distinct car. */
    clear_car(&game->player2);
    get_start(game->selected_track, 5, &game->player2);
    game->player2.model = (uint8_t)((game->selected_car + 3) % 6);
    game->player2.car_type = game->car_type & 1u;
    game->player2.max_speed = car_max_speed[game->player2.car_type][game->player2.model];
    game->camera_angle2 = game->player2.angle;

    for (i = 0; i < SH_AI_CARS; ++i) {
        clear_car(&game->ai[i]);
        get_start(game->selected_track, i + 1, &game->ai[i]);
        game->ai[i].model = (uint8_t)((i + game->selected_car + 1) % 6);
        game->ai[i].car_type = game->car_type & 1u;
        game->ai[i].max_speed = car_max_speed[game->ai[i].car_type][game->ai[i].model];
        game->ai[i].car_speed = (uint8_t)(19 - i);
        game->ai[i].next_point = 0;
        set_ai_target(game->selected_track, &game->ai[i]);
    }
    for (i = 0; i < SH_EFFECTS; ++i) game->effects[i].type = SH_FX_NONE;
    game->collision_count = 0;
    game->wall_collision_count = 0;
    game->car_collision_count = 0;
    game->qa_route = 0;
    game->qa_route_point = 0;
    game->qa_collision_point = 0xFFFFu;
    game->race_ticks = 0;
    game->countdown = 3 * 70 + 69; /* race.c startDelay */
    game->mode = SH_MODE_COUNTDOWN;
}

void sh_game_init(SHGame *game)
{
    unsigned i;
    uint8_t *p = (uint8_t *)game;
    for (i = 0; i < sizeof(*game); ++i) p[i] = 0;
    game->mode = SH_MODE_TITLE;
    game->menu_page = SH_MENU_MAIN;
    game->camera = 0;
    game->hud = 1;
    game->selected_track = 0;
    game->car_type = 0;
    game->selected_car = 0;
}

static void update_lap(uint8_t track, SHCar *car)
{
    SHTrackAssets assets;
    SHPathPoint point;
    int32_t dx, dy;
    uint16_t angle;
    sha_get_track(track, &assets);
    sha_get_path(track, car->npoint, &point);
    dx = (int32_t)((((point.x >> 1) + (1u << 30)) >> 18) - (car->x >> 18));
    dy = (int32_t)((((point.y >> 1) + (1u << 30)) >> 18) - (car->y >> 18));
    angle = sha_vector_angle(dx, -dy);
    if (abs32((int16_t)(angle - (uint16_t)point.direction)) > 0x4000) {
        ++car->npoint;
        if (car->npoint >= assets.path_count) {
            car->npoint = 0;
            if (!car->finished) {
                ++car->nlap;
                if (!car->best_lap || car->lap_time < car->best_lap)
                    car->best_lap = (uint16_t)(car->lap_time > 65535 ? 65535 : car->lap_time);
                car->lap_time = 0;
                if (car->nlap >= SH_LAPS) {
                    car->nlap = SH_LAPS;
                    car->finished = 1;
                    car->v = 0;
                    car->revo = 0;
                }
            }
        }
    }
}

static void add_effect(SHGame *game, uint8_t type, uint32_t x, uint32_t y,
                       uint32_t z, int32_t dx, int32_t dy, uint16_t angle,
                       uint8_t variant)
{
    unsigned i;
    SHEffect *effect = &game->effects[game->frame & (SH_EFFECTS - 1)];
    for (i = 0; i < SH_EFFECTS; ++i)
        if (game->effects[i].type == SH_FX_NONE) { effect = &game->effects[i]; break; }
    effect->x = x; effect->y = y; effect->z = z;
    effect->dx = dx; effect->dy = dy;
    effect->age = 0; effect->angle = angle;
    effect->type = type; effect->variant = variant;
    effect->life = type == SH_FX_SKID ? 210 : (type == SH_FX_SMOKE ? 50 : 30);
}

static void update_effects(SHGame *game)
{
    unsigned i;
    for (i = 0; i < SH_EFFECTS; ++i) {
        SHEffect *effect = &game->effects[i];
        if (effect->type == SH_FX_NONE) continue;
        if (++effect->age >= effect->life) { effect->type = SH_FX_NONE; continue; }
        if (effect->type != SH_FX_SKID) {
            effect->x += (uint32_t)effect->dx;
            effect->y += (uint32_t)effect->dy;
            effect->dx -= effect->dx >> 4;
            effect->dy -= effect->dy >> 4;
            if (effect->type == SH_FX_SMOKE) effect->z += 0x1800;
        }
    }
}

static void car_corners(const SHCar *car, uint32_t x, uint32_t y,
                        uint32_t out_x[4], uint32_t out_y[4])
{
    const int32_t fx = sha_cos30(car->angle);
    const int32_t fy = sha_sin30(car->angle);
    const int32_t lx = -fy;
    const int32_t ly = fx;
    static const int32_t longitudinal[4] = {
        PLAYER_FRONT, PLAYER_FRONT, PLAYER_REAR, PLAYER_REAR
    };
    static const int32_t lateral[4] = {
        PLAYER_HALF_WIDTH, -PLAYER_HALF_WIDTH,
        -PLAYER_HALF_WIDTH, PLAYER_HALF_WIDTH
    };
    unsigned i;
    for (i = 0; i < 4; ++i) {
        out_x[i] = x + (uint32_t)(mul30(longitudinal[i], fx) +
                                  mul30(lateral[i], lx));
        out_y[i] = y + (uint32_t)(mul30(longitudinal[i], fy) +
                                  mul30(lateral[i], ly));
    }
}

static int64_t cross2d(int32_t ax, int32_t ay, int32_t bx, int32_t by,
                       int32_t cx, int32_t cy)
{
    return (int64_t)(bx - ax) * (cy - ay) -
           (int64_t)(by - ay) * (cx - ax);
}

static int segments_intersect(int32_t ax, int32_t ay, int32_t bx, int32_t by,
                              int32_t cx, int32_t cy, int32_t dx, int32_t dy)
{
    int64_t ab_c, ab_d, cd_a, cd_b;
    if ((ax < bx ? bx : ax) < (cx < dx ? cx : dx) ||
        (cx < dx ? dx : cx) < (ax < bx ? ax : bx) ||
        (ay < by ? by : ay) < (cy < dy ? cy : dy) ||
        (cy < dy ? dy : cy) < (ay < by ? ay : by)) return 0;
    ab_c = cross2d(ax, ay, bx, by, cx, cy);
    ab_d = cross2d(ax, ay, bx, by, dx, dy);
    cd_a = cross2d(cx, cy, dx, dy, ax, ay);
    cd_b = cross2d(cx, cy, dx, dy, bx, by);
    return ((ab_c <= 0 && ab_d >= 0) || (ab_d <= 0 && ab_c >= 0)) &&
           ((cd_a <= 0 && cd_b >= 0) || (cd_b <= 0 && cd_a >= 0));
}

static int sector_is_road(const SHTrackAssets *assets, int16_t sector)
{
    SHSector data;
    if (sector < 0 || (uint16_t)sector >= assets->sector_count) return 0;
    sha_get_sector(assets, (uint16_t)sector, &data);
    return data.flags != 0;
}

static int32_t rectangle_release_radius(const SHCar *car, int32_t nx, int32_t ny)
{
    uint32_t corners_x[4], corners_y[4];
    int64_t minimum = 0;
    uint32_t length = isqrt32((uint32_t)(nx * nx + ny * ny));
    unsigned i;
    if (!length) return 48;
    car_corners(car, car->x, car->y, corners_x, corners_y);
    for (i = 0; i < 4; ++i) {
        int32_t dx = (int32_t)(corners_x[i] - car->x) >> 18;
        int32_t dy = (int32_t)(corners_y[i] - car->y) >> 18;
        int64_t projection = (int64_t)dx * nx + (int64_t)dy * ny;
        if (projection < minimum) minimum = projection;
    }
    return sh2_idiv((int32_t)(-minimum + length - 1), (int32_t)length) +
           WALL_RELEASE_MARGIN;
}

static int collide_with_wall(SHGame *game, SHCar *car, uint32_t old_x, uint32_t old_y)
{
    SHTrackAssets assets;
    SHSector old_sector_data;
    SHWall hit_wall;
    uint32_t old_corner_x[4], old_corner_y[4];
    uint32_t new_corner_x[4], new_corner_y[4];
    int32_t bad_x = 0, bad_y = 0;
    const int32_t old_frame_x = (int32_t)old_x >> 18;
    const int32_t old_frame_y = (int32_t)old_y >> 18;
    const int32_t new_frame_x = old_frame_x + ((int32_t)(car->x - old_x) >> 18);
    const int32_t new_frame_y = old_frame_y + ((int32_t)(car->y - old_y) >> 18);
    int16_t old_sector = car->sector;
    int16_t new_sector;
    uint32_t best_distance = 0xFFFFFFFFu;
    int found_wall = 0;
    int has_bad_corner = 0;
    int has_bad_sweep = 0;
    unsigned i, j;

    sha_get_track(game->selected_track, &assets);
    if (old_sector < 0 || (uint16_t)old_sector >= assets.sector_count)
        old_sector = sha_find_sector(&assets, -1, old_x, old_y);
    new_sector = sha_find_sector(&assets, old_sector, car->x, car->y);

    /* userctl.c skips boundary collision when a car was already in a default
     * sector; keep that escape hatch instead of trapping an off-road car. */
    if (!sector_is_road(&assets, old_sector)) {
        car->sector = (int8_t)new_sector;
        car->slidcounter = 0;
        car->slidspeed = 0;
        car->ma = car->angle;
        return 0;
    }

    car_corners(car, old_x, old_y, old_corner_x, old_corner_y);
    car_corners(car, car->x, car->y, new_corner_x, new_corner_y);
    for (i = 0; i < 4; ++i) {
        uint32_t mid_x = old_corner_x[i] +
            (uint32_t)((int32_t)(new_corner_x[i] - old_corner_x[i]) / 2);
        uint32_t mid_y = old_corner_y[i] +
            (uint32_t)((int32_t)(new_corner_y[i] - old_corner_y[i]) / 2);
        int16_t corner_sector = sha_find_sector(&assets, new_sector,
                                                new_corner_x[i], new_corner_y[i]);
        int16_t middle_sector = sha_find_sector(&assets, old_sector, mid_x, mid_y);
        if (!sector_is_road(&assets, corner_sector) && !has_bad_corner) {
            has_bad_corner = 1;
            bad_x = new_frame_x + ((int32_t)(new_corner_x[i] - car->x) >> 18);
            bad_y = new_frame_y + ((int32_t)(new_corner_y[i] - car->y) >> 18);
        }
        if (!sector_is_road(&assets, middle_sector)) has_bad_sweep = 1;
    }

    sha_get_sector(&assets, (uint16_t)old_sector, &old_sector_data);
    for (i = 0; i < old_sector_data.side_count; ++i) {
        SHSectorSide side;
        SHWall wall;
        int32_t x0, y0, x1, y1, vx, vy, length2;
        int geometry_hit = 0;
        sha_get_sector_side(&assets, old_sector_data.first_side + i, &side);
        if (sector_is_road(&assets, side.other)) continue;
        if (side.wall != 0xFFFFu) {
            sha_get_wall(game->selected_track, side.wall, &wall);
        } else {
            SHSectorVertex v0, v1;
            /* The DOS fallback still blocks an untextured road/default edge.
             * Build its world endpoints directly from complete SEC topology. */
            sha_get_sector_vertex(&assets, side.v0, &v0);
            sha_get_sector_vertex(&assets, side.v1, &v1);
            wall.x0 = ((uint32_t)v0.x << 15) + 0x40000000u;
            wall.y0 = ((uint32_t)v0.y << 15) + 0x40000000u;
            wall.x1 = ((uint32_t)v1.x << 15) + 0x40000000u;
            wall.y1 = ((uint32_t)v1.y << 15) + 0x40000000u;
            wall.texture = 0xFFFFu;
            wall.flags = SH_WALL_COLLIDABLE;
        }
        /* Keep walls and the swept rectangle in one frame centred on the old
         * car position. This also unwraps a car crossing the signed seam. */
        x0 = old_frame_x + ((int32_t)(wall.x0 - old_x) >> 18);
        y0 = old_frame_y + ((int32_t)(wall.y0 - old_y) >> 18);
        /* World coordinates wrap at signed 0x80000000. Subtract while still
         * unsigned, then sign-extend the short wall delta. Converting both
         * endpoints separately creates a phantom ~16384-unit wall whenever a
         * real side crosses that seam (the Racer's Edge backward-push bug). */
        vx = ((int32_t)(wall.x1 - wall.x0)) >> 18;
        vy = ((int32_t)(wall.y1 - wall.y0)) >> 18;
        x1 = x0 + vx; y1 = y0 + vy;
        length2 = vx * vx + vy * vy;
        if (length2 <= 0) continue;

        /* Endpoint rectangle edges plus each corner's swept path prevent a
         * fast tick from tunnelling through a thin boundary polygon. */
        for (j = 0; j < 4 && !geometry_hit; ++j) {
            unsigned next = (j + 1u) & 3u;
            int32_t nx0 = new_frame_x + ((int32_t)(new_corner_x[j] - car->x) >> 18);
            int32_t ny0 = new_frame_y + ((int32_t)(new_corner_y[j] - car->y) >> 18);
            int32_t nx1 = new_frame_x + ((int32_t)(new_corner_x[next] - car->x) >> 18);
            int32_t ny1 = new_frame_y + ((int32_t)(new_corner_y[next] - car->y) >> 18);
            int32_t ox = old_frame_x + ((int32_t)(old_corner_x[j] - old_x) >> 18);
            int32_t oy = old_frame_y + ((int32_t)(old_corner_y[j] - old_y) >> 18);
            geometry_hit = (has_bad_corner || has_bad_sweep) &&
                           segments_intersect(x0, y0, x1, y1,
                                              ox, oy, nx0, ny0);
            /* Match userctl.c: an endpoint rectangle edge only blocks when a
             * corner actually resolves to default/outside. Road polygons can
             * overlap at joins, so edge-only contact there is not a wall. */
            if (!geometry_hit && has_bad_corner)
                geometry_hit = segments_intersect(x0, y0, x1, y1,
                                                  nx0, ny0, nx1, ny1);
        }
        if (geometry_hit) {
            hit_wall = wall;
            found_wall = 1;
            break;
        }
        if (has_bad_corner) {
            int32_t qx, qy, dx, dy;
            uint32_t distance;
            nearest_wall_point(bad_x, bad_y, x0, y0, vx, vy, length2, &qx, &qy);
            dx = bad_x - qx; dy = bad_y - qy;
            distance = (uint32_t)(dx * dx + dy * dy);
            if (distance < best_distance) {
                best_distance = distance;
                hit_wall = wall;
                found_wall = 1;
            }
        }
    }

    if (found_wall && (has_bad_corner || best_distance == 0xFFFFFFFFu)) {
        int32_t x0 = old_frame_x + ((int32_t)(hit_wall.x0 - old_x) >> 18);
        int32_t y0 = old_frame_y + ((int32_t)(hit_wall.y0 - old_y) >> 18);
        int32_t vx = ((int32_t)(hit_wall.x1 - hit_wall.x0)) >> 18;
        int32_t vy = ((int32_t)(hit_wall.y1 - hit_wall.y0)) >> 18;
        int32_t length2 = vx * vx + vy * vy;
        int32_t old_cx = old_frame_x;
        int32_t old_cy = old_frame_y;
        int32_t qx, qy, nx = -vy, ny = vx;
        int32_t radius, old_distance;
        int64_t side_dot, normal_motion;
        uint16_t wall_angle;
        int16_t reflection;

        nearest_wall_point(old_cx, old_cy, x0, y0, vx, vy, length2, &qx, &qy);
        side_dot = (int64_t)(old_cx - x0) * nx + (int64_t)(old_cy - y0) * ny;
        if (side_dot < 0) { nx = -nx; ny = -ny; }
        else if (side_dot == 0) {
            int32_t attempted_x = (int32_t)(car->x - old_x) >> 18;
            int32_t attempted_y = (int32_t)(car->y - old_y) >> 18;
            if ((int64_t)attempted_x * nx + (int64_t)attempted_y * ny > 0) {
                nx = -nx; ny = -ny;
            }
        }
        radius = rectangle_release_radius(car, nx, ny);
        normal_motion = (int64_t)(int32_t)(car->x - old_x) * nx +
                        (int64_t)(int32_t)(car->y - old_y) * ny;

        if (normal_motion >= 0) {
            /* A rectangle still overlapping while moving out or parallel is
             * depenetrated once, but never reflected back into the guardrail. */
            int32_t cx = new_frame_x;
            int32_t cy = new_frame_y;
            nearest_wall_point(cx, cy, x0, y0, vx, vy, length2, &qx, &qy);
            release_from_wall(car, qx, qy, nx, ny, radius);
            car->sector = (int8_t)sha_find_sector(&assets, old_sector,
                                                  car->x, car->y);
            return 0;
        }

        car->x = old_x; car->y = old_y;
        old_distance = sh2_idiv((int32_t)((int64_t)(old_cx - x0) * nx +
                                             (int64_t)(old_cy - y0) * ny),
                                (int32_t)isqrt32((uint32_t)length2));
        if (old_distance < radius)
            release_from_wall(car, qx, qy, nx, ny, radius);
        car->sector = (int8_t)sha_find_sector(&assets, old_sector,
                                              car->x, car->y);

        wall_angle = sha_vector_angle(vx, -vy);
        reflection = (int16_t)(2 * (wall_angle - car->ma));
        car->ma = (uint16_t)(car->ma + reflection);
        car->slidspeed = car->v >> 1;
        car->slidcounter = (int16_t)(10 +
            muldiv(abs32(car->v >> 14), abs32(reflection), 0x10000));
        car->slidva = (int16_t)muldiv(reflection >> 4, car->slidspeed,
                                      1 << 22);
        car->sliding = 1;
        car->revo -= car->revo / 50;
        ++game->collision_count;
        ++game->wall_collision_count;
#ifdef ENABLE_QA_HOOKS
        if (game->qa_route == 1 && game->qa_collision_point == 0xFFFFu)
            game->qa_collision_point = game->qa_route_point;
#endif
        add_effect(game, SH_FX_SPARK, car->x, car->y, 0x40000,
                   0, 0, wall_angle, 0);
        return 1;
    }

    car->sector = (int8_t)new_sector;
    return 0;
}

static void player_tick(SHGame *game, SHCar *p, uint16_t pad, int race_started)
{
    const uint8_t track = game->selected_track;
    int32_t a, maxva, maxa;
    uint32_t old_x, old_y;
    const int32_t reference_speed = class_reference_speed(p->car_type);
    int32_t dx, dy;
    int braking = 0;
    const int accelerate = pad & (SH_PAD_UP | SH_PAD_B | SH_PAD_C);
    const int brake = pad & (SH_PAD_DOWN | SH_PAD_A);

    ++p->total_time;
    ++p->lap_time;

    if (accelerate) {
        if (gears[p->gear].ratio >= 0)
            p->revo += muldiv(gears[p->gear].accel, p->max_speed, reference_speed);
        else if (race_started) {
            p->revo -= gears[p->gear].accel;
            if (p->revo <= 0) ++p->gear;
        }
    }

    p->v = muldiv(p->v, reference_speed, p->max_speed);
    if (brake) {
        braking = 1;
        if (gears[p->gear].ratio > 0) {
            int32_t decel = (1 << 12) + (p->v >> 8);
            p->revo = (int32_t)(((int64_t)(p->v - decel) << 16) / gears[p->gear].ratio);
        } else if (race_started && p->gear > 0 && gears[p->gear].ratio == 0) {
            --p->gear;
            p->revo += gears[p->gear].accel;
        } else if (gears[p->gear].ratio < 0) {
            p->revo += gears[p->gear].accel;
        }
    }
    p->revo -= gears[p->gear].accel / 8;
    if (p->revo <= 0) { p->gear = 1; p->revo = 0; }

    if (race_started && p->automatic && p->gear < 7 && p->revo > gears[p->gear].maxrevo) {
        ++p->gear;
        /* userctl.c derives RPM from road speed after a shift. At launch the
         * neutral ratio makes road speed zero; retaining the accumulated RPM
         * here is the clutch engagement that the omitted DOS input layer supplied. */
        if (gears[p->gear].ratio > 0 && p->v != 0)
            p->revo = (int32_t)(((int64_t)p->v << 16) / gears[p->gear].ratio);
    }
    if (race_started && p->automatic && p->gear > 2 && p->revo < gears[p->gear].minrevo) {
        int32_t r = (int32_t)(((int64_t)p->v << 16) / gears[p->gear - 1].ratio);
        if (r <= MAX_REVO) { p->revo = r; --p->gear; }
    }
    if (p->revo > MAX_REVO) p->revo = MAX_REVO;

    {
        int32_t s = p->v << 8;
        if (s < 0) s = 0;
        maxva = 40 - mul30(45, s);
        if (maxva < 7) maxva = 7;
        maxa = 8 * (120 - mul30(160, s));
        if (maxa < 25 * 8) maxa = 25 * 8;
    }
    a = maxva;
    if (pad & SH_PAD_LEFT) { if (p->va < 0) p->va = 0; }
    if (pad & SH_PAD_RIGHT) { if (p->va > 0) p->va = 0; a = -a; }
    if (!(pad & (SH_PAD_LEFT | SH_PAD_RIGHT))) a = 0;
    p->va = (int16_t)(p->va + a);
    if (p->va < -maxa) p->va = (int16_t)-maxa;
    if (p->va > maxa) p->va = (int16_t)maxa;
    if (!(pad & (SH_PAD_LEFT | SH_PAD_RIGHT))) p->va /= 2;

    p->v = (int32_t)(((int64_t)p->revo * gears[p->gear].ratio) >> 16);
    p->v = muldiv(p->v, p->max_speed, reference_speed);

    /* userctl.c gives a crash a short out-of-control phase before normal
     * traction recovery. This rotates the body toward the reflected movement
     * and, importantly, leaves steering active so the player can turn away. */
    if (p->slidcounter > 0) {
        p->sliding = 1;
        p->revo -= p->revo >> 6;
        --p->slidcounter;
        p->angle = (uint16_t)(p->angle + p->slidva);
        if (p->slidva > 2) p->slidva -= 2;
        else if (p->slidva < -2) p->slidva += 2;
        p->slidspeed -= p->slidspeed >> 7;
    /* userctl.c only enables driver-initiated powersliding for Stock cars. */
    } else if (p->car_type == 1 && p->v > (3 << 19) &&
               abs32((1 + 2 * braking) * p->va) >= abs32(maxa) - 8) {
        p->sliding = 1;
        p->ma = (uint16_t)(p->ma + muldiv(p->va, p->v, 1 << (22 + braking)));
        p->angle = (uint16_t)(p->angle +
                   muldiv(p->va * (1 + braking) / 2, p->v, 1 << 22));
        p->slidspeed -= mul30(p->slidspeed >> 9,
                              sha_cos30((uint16_t)(p->angle - p->ma))) +
                         (p->slidspeed >> 9);
        p->slidva = p->va;
    } else if (p->sliding) {
        int16_t correction;
        p->slidspeed += mul30((p->v - p->slidspeed) / 128,
                              sha_cos30((uint16_t)(p->angle - p->ma)));
        correction = (int16_t)(p->angle - p->ma) / 32;
        p->ma = (uint16_t)(p->ma + correction);
        if (abs32(correction) < 16 && abs32(p->v - p->slidspeed) < (1 << 19))
            p->sliding = 0;
    } else {
        p->ma = p->angle;
        p->slidspeed = p->v;
        p->slidva = 0;
    }

    dx = mul30(p->slidspeed, sha_cos30(p->ma));
    dy = mul30(p->slidspeed, sha_sin30(p->ma));
    /* This turn is unconditional in userctl.c, including crash/powerslide
     * states. Suppressing it while sliding made steering away impossible. */
    p->angle = (uint16_t)(p->angle + muldiv(p->va, p->v, 1 << 22));
    p->tirerot = (uint16_t)(p->tirerot + p->v / 512);
    p->tiredir = (uint16_t)(-4 * p->va - (p->v ? muldiv(16 * p->va, p->v, 1 << 22) : 0));
    if (p->slidcounter <= 0) {
        if (abs32(p->v) > (1 << 9)) p->movangle = sha_vector_angle(dx, -dy);
        else p->movangle = p->angle;
    }
    old_x = p->x; old_y = p->y;
    p->x += (uint32_t)dx;
    p->y += (uint32_t)dy;
    collide_with_wall(game, p, old_x, old_y);

    /* racemap.c/userctl.c use palette indices 160..191 as driveable asphalt. */
    {
        uint8_t ground = sh_ground_color(track, p->x, p->y);
        if (ground < 160 || ground >= 192) {
            p->revo -= p->revo >> 7;
            p->z = 0x30000;
            if ((game->race_ticks & 7u) == 0 && abs32(p->v) > (1 << 18))
                add_effect(game, SH_FX_SMOKE, p->x, p->y, p->z,
                           dx >> 2, dy >> 2, p->movangle,
                           ground >= 48 && ground < 80 ? 1 : 0);
        } else p->z = 0x20000;
    }
    if ((p->sliding || (braking && abs32(p->va) > 120)) &&
        abs32(p->v) > (2 << 19) && (game->race_ticks & 3u) == 0)
        add_effect(game, SH_FX_SKID, p->x, p->y, 0x18000,
                   0, 0, p->movangle, 0);
    update_lap(track, p);
}

#ifdef ENABLE_QA_HOOKS
static void qa_route_tick(SHGame *game)
{
    SHTrackAssets assets;
    SHPathPoint point;
    SHCar *car = &game->player;
    uint32_t target_x, target_y, old_x, old_y;
    int32_t dx, dy, sx, sy, ax, ay, distance;
    const int32_t speed = 3 << 19;
    if (game->qa_route != 1) return;
    sha_get_track(game->selected_track, &assets);
    if (game->qa_route_point >= assets.path_count) {
        game->qa_route = 2;
        car->nlap = 1;
        return;
    }
    sha_get_path(game->selected_track, game->qa_route_point, &point);
    target_x = (point.x >> 1) + (1u << 30);
    target_y = (point.y >> 1) + (1u << 30);
    dx = (int32_t)(target_x - car->x);
    dy = (int32_t)(target_y - car->y);
    sx = dx >> 18; sy = dy >> 18;
    ax = abs32(sx); ay = abs32(sy);
    distance = (ax > ay ? ax : ay) + ((ax > ay ? ay : ax) >> 1);
    if (distance <= (speed >> 18) + 1) {
        car->x = target_x; car->y = target_y;
        car->sector = (int8_t)sha_find_sector(&assets, car->sector, car->x, car->y);
        ++game->qa_route_point;
        return;
    }
    car->angle = sha_vector_angle(sx, -sy);
    car->ma = car->movangle = car->angle;
    car->v = car->slidspeed = speed;
    car->sliding = 0;
    car->slidcounter = 0;
    old_x = car->x; old_y = car->y;
    car->x += (uint32_t)mul30(speed, sha_cos30(car->angle));
    car->y += (uint32_t)mul30(speed, sha_sin30(car->angle));
    collide_with_wall(game, car, old_x, old_y);
}
#endif

static void ai_tick(SHGame *game, SHCar *car)
{
    const uint8_t track = game->selected_track;
    SHPathPoint point;
    int32_t dx, dy, d2;
    int16_t delta;

    if (car->finished) return;
    car->angle = (uint16_t)(car->angle + muldiv(car->av, car->v, 1 << 22));
    car->x += (uint32_t)mul30(car->v, sha_cos30(car->angle));
    car->y += (uint32_t)mul30(car->v, sha_sin30(car->angle));
    /* The car physically moves along its body angle; expose that as the
     * movement heading so the split-screen chase camera (which lags 1/16
     * toward movangle) turns to follow this AI car instead of freezing. */
    car->movangle = car->angle;
    {
        SHTrackAssets assets;
        sha_get_track(track, &assets);
        car->sector = (int8_t)sha_find_sector(&assets, car->sector, car->x, car->y);
    }
    /* cars.c keeps AI on its PATH and does not run the player's rectangular
     * boundary pass. It still tracks its current SEC polygon for topology. */
    update_lap(track, car);

    dx = (int32_t)((car->target_x >> 18) - (car->x >> 18));
    dy = (int32_t)((car->target_y >> 18) - (car->y >> 18));
    d2 = dx * dx + dy * dy;
    if (d2 < (1 << 15) ||
        (d2 < (1 << 19) && abs32((int16_t)(sha_vector_angle(dx, -dy) - car->angle)) > 0x4000)) {
        SHTrackAssets assets;
        sha_get_track(track, &assets);
        car->next_point = (uint8_t)((car->next_point + 1) % assets.path_count);
        set_ai_target(track, car);
        dx = (int32_t)((car->target_x >> 18) - (car->x >> 18));
        dy = (int32_t)((car->target_y >> 18) - (car->y >> 18));
    }
    car->to_angle = sha_vector_angle(dx, -dy);
    delta = (int16_t)(car->to_angle - car->angle + car->av);
    {
        int32_t s = -sha_sin30((uint16_t)(car->v >> 8));
        int32_t max_turn_speed = 40 - mul30(30, s);
        int32_t max_turn = 120 - mul30(105, s);
        if (delta < 0) car->av -= (int16_t)(car->av > 0 ? 2 * max_turn_speed : max_turn_speed);
        else if (delta > 0) car->av += (int16_t)(car->av < 0 ? 2 * max_turn_speed : max_turn_speed);
        if (car->av < -max_turn * 8) car->av = (int16_t)(-max_turn * 8);
        if (car->av > max_turn * 8) car->av = (int16_t)(max_turn * 8);
    }
    sha_get_path(track, car->next_point, &point);
    car->to_speed = point.speed - 1800 * 256;
    if (car->to_speed < car->v)
        car->v -= car->car_type ? (0x8000 * 3 / 4) : 0x8000;
    else if (car->to_speed > car->v) {
        int32_t acceleration = car->car_type ? 0x1000 :
                               0x1100 - muldiv(0xC00, car->v, 1 << 22);
        car->v += acceleration * (car->car_speed + 3) / 16;
    }
    car->tirerot = (uint16_t)(car->tirerot + abs32(car->v / 512));
    car->tiredir = (uint16_t)(-4 * car->av - (car->v ? muldiv(16 * car->av, car->v, 1 << 22) : 0));
    {
        uint8_t ground = sh_ground_color(track, car->x, car->y);
        if (ground < 160 || ground >= 192) car->v -= car->v >> 7;
    }
}

/* Oriented rectangular car-to-car collision via the Separating Axis Theorem.
 * Each car is the asymmetric PlayerBounds box (0xB00000 front, -0x800000
 * rear, +/-0x380000 width) rotated by its heading. This replaces the circle
 * approximation with correct side-swipe / rear / head-on normals and an exact
 * minimum-translation vector that fully separates the pair. */
static void car_rect_corners_i(const SHCar *car, int32_t cx, int32_t cy,
                               int32_t out_x[4], int32_t out_y[4])
{
    const int32_t fx = sha_cos30(car->angle);
    const int32_t fy = sha_sin30(car->angle);
    const int32_t lx = -fy, ly = fx;
    static const int32_t lon[4] = {
        PLAYER_FRONT, PLAYER_FRONT, PLAYER_REAR, PLAYER_REAR
    };
    static const int32_t lat[4] = {
        PLAYER_HALF_WIDTH, -PLAYER_HALF_WIDTH,
        -PLAYER_HALF_WIDTH, PLAYER_HALF_WIDTH
    };
    unsigned i;
    for (i = 0; i < 4; ++i) {
        out_x[i] = cx + ((mul30(lon[i], fx) + mul30(lat[i], lx)) >> 18);
        out_y[i] = cy + ((mul30(lon[i], fy) + mul30(lat[i], ly)) >> 18);
    }
}

/* Project both rectangle corner sets onto the 2.30 axis. Returns the interval
 * overlap in world units, or -1 when the axis fully separates them. */
static int32_t axis_overlap(int32_t ax, int32_t ay,
                            const int32_t *axs, const int32_t *ays,
                            const int32_t *bxs, const int32_t *bys)
{
    int32_t minA = 0x7FFFFFFF, maxA = -0x7FFFFFFF;
    int32_t minB = 0x7FFFFFFF, maxB = -0x7FFFFFFF;
    int j;
    for (j = 0; j < 4; ++j) {
        int32_t p = (int32_t)(((int64_t)ax * axs[j] + (int64_t)ay * ays[j]) >> 30);
        if (p < minA) minA = p;
        if (p > maxA) maxA = p;
        p = (int32_t)(((int64_t)ax * bxs[j] + (int64_t)ay * bys[j]) >> 30);
        if (p < minB) minB = p;
        if (p > maxB) maxB = p;
    }
    if (maxA < minB || maxB < minA) return -1;
    return (maxA < maxB ? maxA : maxB) - (minA > minB ? minA : minB);
}

static void collide_cars(SHGame *game)
{
    SHCar *player = &game->player;
    const int32_t pf = sha_cos30(player->angle);
    const int32_t ps = sha_sin30(player->angle);
    unsigned i;
    for (i = 0; i < SH_AI_CARS; ++i) {
        SHCar *other = &game->ai[i];
        int32_t ocx = ((int32_t)(other->x - player->x)) >> 18;
        int32_t ocy = ((int32_t)(other->y - player->y)) >> 18;
        int32_t pax[4], pay[4], obx[4], oby[4];
        const int32_t of = sha_cos30(other->angle);
        const int32_t os = sha_sin30(other->angle);
        int32_t axes[4][2];
        int32_t min_overlap = 0x7FFFFFFF;
        int32_t mtv_x = 0, mtv_y = 0;
        int a, separated = 0;
        /* Early reject: centres further than the widest box diagonal. */
        if (abs32(ocx) > 120 || abs32(ocy) > 120) continue;
        car_rect_corners_i(player, 0, 0, pax, pay);
        car_rect_corners_i(other, ocx, ocy, obx, oby);
        axes[0][0] = pf;  axes[0][1] = ps;   /* player forward */
        axes[1][0] = -ps; axes[1][1] = pf;   /* player lateral */
        axes[2][0] = of;  axes[2][1] = os;   /* other forward */
        axes[3][0] = -os; axes[3][1] = of;   /* other lateral */
        for (a = 0; a < 4; ++a) {
            int32_t ov = axis_overlap(axes[a][0], axes[a][1],
                                      pax, pay, obx, oby);
            if (ov < 0) { separated = 1; break; }
            if (ov < min_overlap) { min_overlap = ov; mtv_x = axes[a][0]; mtv_y = axes[a][1]; }
        }
        if (separated) continue;
        {
            /* MTV axis is the least-overlap separating axis. Push the player
             * AWAY from the other car's centre: when cd>0 the other car lies
             * on the +mtv side, so the player moves in the -mtv direction. */
            int64_t cd = (int64_t)ocx * mtv_x + (int64_t)ocy * mtv_y;
            int32_t ux = mtv_x, uy = mtv_y;
            int32_t push_x, push_y;
            if (cd > 0) { ux = -ux; uy = -uy; }
            push_x = (int32_t)(((int64_t)min_overlap * ux) >> 30);
            push_y = (int32_t)(((int64_t)min_overlap * uy) >> 30);
            if (push_x == 0 && push_y == 0) push_x = (ux >= 0) ? 1 : -1;
            player->x += (uint32_t)(push_x << 18);
            player->y += (uint32_t)(push_y << 18);
        }
        player->revo = (player->revo * 7) >> 3;
        player->slidspeed = player->v >> 1;
        player->slidcounter = 12;
        player->sliding = 1;
        other->v = (other->v * 7) >> 3;
        ++game->collision_count;
        ++game->car_collision_count;
        if ((game->race_ticks & 3u) == 0)
            add_effect(game, SH_FX_SPARK,
                       player->x / 2u + other->x / 2u,
                       player->y / 2u + other->y / 2u,
                       0x40000, 0, 0, player->movangle, 0);
    }
}

static int32_t racer_progress(const SHCar *car)
{
    return (int32_t)car->nlap * 10000 + car->npoint;
}

static void rank_cars(SHGame *game)
{
    const SHCar *racers[SH_RACERS];
    unsigned i, j;
    racers[0] = &game->player;
    for (i = 0; i < SH_AI_CARS; ++i) racers[i + 1] = &game->ai[i];
    racers[SH_AI_CARS + 1] = &game->player2;
    game->player.position = 1;
    for (i = 0; i < SH_AI_CARS; ++i) game->ai[i].position = 1;
    game->player2.position = 1;
    for (i = 0; i < SH_RACERS; ++i)
        for (j = 0; j < SH_RACERS; ++j)
            if (racer_progress(racers[j]) > racer_progress(racers[i])) {
                if (i == 0) ++game->player.position;
                else if (i <= SH_AI_CARS) ++game->ai[i - 1].position;
                else ++game->player2.position;
            }
}

/* race.c::HandleView(): chase/high cameras lag the car's movement angle by
 * 1/16; cockpit uses the physical body angle. update_camera() drives the two
 * per-player camera angles independently so split screen follows each car. */
static void update_one_camera(SHGame *game, const SHCar *car, uint16_t *angle)
{
    if (game->camera == 1) {
        *angle = car->angle;
    } else {
        int16_t delta = (int16_t)(car->movangle - *angle);
        int16_t step = delta / 16;
        if (step == 0) *angle = car->movangle;
        else *angle = (uint16_t)(*angle + step);
    }
}

static void update_camera(SHGame *game)
{
    update_one_camera(game, &game->player, &game->camera_angle);
    if (game->split)
        update_one_camera(game, &game->player2, &game->camera_angle2);
}

static void drive_player2(SHGame *game, uint16_t pad2)
{
    /* Human control only while a second controller is physically present.
     * Otherwise the computer drives P2. Using presence (not "any button held")
     * stops the AI from grabbing the car every time the human releases input. */
    if (game->p2_human)
        player_tick(game, &game->player2, pad2, 1);
    else
        ai_tick(game, &game->player2);  /* no second pad: computer drives P2 */
}

static void simulation_tick(SHGame *game, uint16_t pad, uint16_t pad2)
{
    unsigned i;
    if (game->mode == SH_MODE_COUNTDOWN) {
        if (game->countdown) --game->countdown;
        if (game->countdown < 70) {
            player_tick(game, &game->player, pad, 1);
            if (game->split) drive_player2(game, pad2);
            for (i = 0; i < SH_AI_CARS; ++i)
                ai_tick(game, &game->ai[i]);
            collide_cars(game);
            update_effects(game);
        }
        update_camera(game);
        if (!game->countdown) game->mode = SH_MODE_RACE;
        return;
    }
    if (game->mode != SH_MODE_RACE) return;
    ++game->race_ticks;
#ifdef ENABLE_QA_HOOKS
    if (game->qa_route) qa_route_tick(game);
    else
#endif
        player_tick(game, &game->player, pad, 1);
    if (game->split) drive_player2(game, pad2);
    for (i = 0; i < SH_AI_CARS; ++i)
        ai_tick(game, &game->ai[i]);
#ifdef ENABLE_QA_HOOKS
    if (!game->qa_route)
#endif
        collide_cars(game);
    update_effects(game);
    rank_cars(game);
    update_camera(game);
    if (game->player.finished) game->mode = SH_MODE_FINISHED;
}

void sh_game_frame(SHGame *game, uint16_t pad, uint16_t pad2,
                   int p2_present, uint16_t elapsed_vblanks)
{
    uint16_t pressed = pad & (uint16_t)~game->previous_pad;
    game->p2_human = p2_present ? 1 : 0;
    if (elapsed_vblanks == 0) elapsed_vblanks = 1;
    if (elapsed_vblanks > 30) elapsed_vblanks = 30;
    game->previous_pad = pad;
    game->frame += elapsed_vblanks;

    if (game->mode == SH_MODE_MENU && game->menu_latched) {
        const uint16_t menu_mask = SH_PAD_UP | SH_PAD_DOWN | SH_PAD_LEFT |
                                   SH_PAD_RIGHT | SH_PAD_A | SH_PAD_B |
                                   SH_PAD_C | SH_PAD_START;
        if ((pad & menu_mask) == 0) {
            unsigned released = game->menu_release + elapsed_vblanks;
            game->menu_release = (uint8_t)(released > 255 ? 255 : released);
            if (game->menu_release >= 8) {
                game->menu_latched = 0;
                game->menu_release = 0;
            }
        } else {
            game->menu_release = 0;
        }
        pressed &= (uint16_t)~menu_mask;
    }

    if (pressed & SH_PAD_X) game->camera = (uint8_t)((game->camera + 1) & 3u);
    if (pressed & SH_PAD_Y) game->hud ^= 1;

    if (game->mode == SH_MODE_TITLE &&
        (pressed & (SH_PAD_START | SH_PAD_A | SH_PAD_B | SH_PAD_C))) {
        game->mode = SH_MODE_MENU;
        game->menu_page = SH_MENU_MAIN;
        game->menu_latched = 1;
        game->menu_release = 0;
    } else if (game->mode == SH_MODE_MENU) {
        uint16_t menu_press = pressed & (SH_PAD_UP | SH_PAD_DOWN | SH_PAD_LEFT |
                                         SH_PAD_RIGHT | SH_PAD_A | SH_PAD_B |
                                         SH_PAD_C | SH_PAD_START);
        uint16_t confirm = pressed & (SH_PAD_START | SH_PAD_B | SH_PAD_C);
        if (pressed & SH_PAD_A) {
            if (game->menu_page == SH_MENU_MAIN) game->mode = SH_MODE_TITLE;
            else --game->menu_page;
        } else if (game->menu_page == SH_MENU_MAIN) {
            if (pressed & (SH_PAD_LEFT | SH_PAD_RIGHT))
                game->split ^= 1u;   /* one / two player */
            if (confirm) game->menu_page = SH_MENU_CIRCUIT;
        } else if (game->menu_page == SH_MENU_CIRCUIT) {
            if (pressed & (SH_PAD_LEFT | SH_PAD_RIGHT | SH_PAD_UP | SH_PAD_DOWN))
                game->selected_track ^= 1u;
            if (confirm) game->menu_page = SH_MENU_CLASS;
        } else if (game->menu_page == SH_MENU_CLASS) {
            if (pressed & (SH_PAD_LEFT | SH_PAD_RIGHT | SH_PAD_UP | SH_PAD_DOWN))
                game->car_type ^= 1u;
            if (confirm) game->menu_page = SH_MENU_CAR;
        } else {
            if (pressed & (SH_PAD_LEFT | SH_PAD_UP))
                game->selected_car = (uint8_t)((game->selected_car + 5) % 6);
            if (pressed & (SH_PAD_RIGHT | SH_PAD_DOWN))
                game->selected_car = (uint8_t)((game->selected_car + 1) % 6);
            if (confirm) reset_race(game);
        }
        if (game->mode == SH_MODE_MENU && menu_press) {
            game->menu_latched = 1;
            game->menu_release = 0;
        }
    } else if (game->mode == SH_MODE_RACE && (pressed & SH_PAD_START))
        game->mode = SH_MODE_PAUSED;
#ifdef ENABLE_QA_HOOKS
    else if (game->mode == SH_MODE_RACE &&
             (pad & (SH_PAD_LEFT | SH_PAD_RIGHT | SH_PAD_B)) ==
             (SH_PAD_LEFT | SH_PAD_RIGHT | SH_PAD_B) &&
             (pressed & SH_PAD_B)) {
        /* Impossible D-pad chord: drive the authored PATH centreline through
         * real rectangular collision for the Racer's Edge seam regression. */
        game->qa_route = 1;
        game->qa_route_point = 0;
        game->qa_collision_point = 0xFFFFu;
        game->collision_count = 0;
        game->wall_collision_count = 0;
    }
    else if (game->mode == SH_MODE_RACE &&
             (pad & (SH_PAD_LEFT | SH_PAD_RIGHT | SH_PAD_C)) ==
             (SH_PAD_LEFT | SH_PAD_RIGHT | SH_PAD_C) &&
             (pressed & SH_PAD_C)) {
        /* Left+Right is impossible on a physical D-pad. The point-to-point
         * test advances one timing-line result while retaining the finish UI. */
        if (++game->player.nlap >= SH_LAPS) {
            game->player.nlap = SH_LAPS;
            game->player.finished = 1;
            game->mode = SH_MODE_FINISHED;
        }
    }
    else if (game->mode == SH_MODE_RACE &&
             (pad & (SH_PAD_LEFT | SH_PAD_RIGHT | SH_PAD_A)) ==
             (SH_PAD_LEFT | SH_PAD_RIGHT | SH_PAD_A) &&
             (pressed & SH_PAD_A)) {
        /* QA: force a rectangular car-to-car collision by overlapping an AI
         * car with the player, then verify the SAT MTV fires (car probe). */
        game->ai[0].x = game->player.x;
        game->ai[0].y = game->player.y;
        game->ai[0].angle = game->player.angle;
        game->car_collision_count = 0;
    }
#endif
    else if (game->mode == SH_MODE_PAUSED && (pressed & SH_PAD_START))
        game->mode = SH_MODE_RACE;
    else if (game->mode == SH_MODE_FINISHED &&
             (pressed & (SH_PAD_START | SH_PAD_A | SH_PAD_B | SH_PAD_C))) {
        game->mode = SH_MODE_MENU;
        game->menu_page = SH_MENU_MAIN;
        game->menu_latched = 1;
        game->menu_release = 0;
    }

    /* race.c runs as many 70 Hz world simulations as the timer says are due.
     * Rendering can span several VBlanks on 32X, so never tie vehicle speed to
     * completed video frames. */
    {
        uint32_t accumulator = game->sim_accumulator + (uint32_t)70 * elapsed_vblanks;
        while (accumulator >= 60) {
            accumulator -= 60;
            simulation_tick(game, pad, pad2);
        }
        game->sim_accumulator = (uint16_t)accumulator;
    }
}
