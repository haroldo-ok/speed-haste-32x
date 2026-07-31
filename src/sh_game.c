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
#define WALL_COLLISION_RADIUS 32
#define WALL_RELEASE_RADIUS 36

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
                              int32_t nx, int32_t ny)
{
    uint32_t distance = isqrt32((uint32_t)(nx * nx + ny * ny));
    int32_t rx, ry;
    if (!distance) return;
    rx = qx + muldiv(nx, WALL_RELEASE_RADIUS, (int32_t)distance);
    ry = qy + muldiv(ny, WALL_RELEASE_RADIUS, (int32_t)distance);
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

static int collide_with_wall(SHGame *game, SHCar *car, uint32_t old_x, uint32_t old_y)
{
    SHTrackAssets assets;
    const int32_t cx = ((int32_t)car->x) >> 18;
    const int32_t cy = ((int32_t)car->y) >> 18;
    const int32_t old_cx = ((int32_t)old_x) >> 18;
    const int32_t old_cy = ((int32_t)old_y) >> 18;
    unsigned i;
    sha_get_track(game->selected_track, &assets);
    for (i = 0; i < assets.wall_count; ++i) {
        const uint8_t *record = assets.walls + i * 20u;
        int32_t x0, y0, vx, vy, length2, qx, qy, dx, dy;
        if (!(sha_rd16(record + 18) & SH_WALL_COLLIDABLE)) continue;
        x0 = ((int32_t)sha_rd32(record)) >> 18;
        y0 = ((int32_t)sha_rd32(record + 4)) >> 18;
        vx = ((int32_t)(sha_rd32(record + 8) - sha_rd32(record))) >> 18;
        vy = ((int32_t)(sha_rd32(record + 12) - sha_rd32(record + 4))) >> 18;
        if (cx < (x0 < x0 + vx ? x0 : x0 + vx) - WALL_RELEASE_RADIUS ||
            cx > (x0 > x0 + vx ? x0 : x0 + vx) + WALL_RELEASE_RADIUS ||
            cy < (y0 < y0 + vy ? y0 : y0 + vy) - WALL_RELEASE_RADIUS ||
            cy > (y0 > y0 + vy ? y0 : y0 + vy) + WALL_RELEASE_RADIUS) continue;
        length2 = vx * vx + vy * vy;
        if (length2 <= 0) continue;
        nearest_wall_point(cx, cy, x0, y0, vx, vy, length2, &qx, &qy);
        dx = cx - qx; dy = cy - qy;
        if (dx * dx + dy * dy < WALL_COLLISION_RADIUS * WALL_COLLISION_RADIUS) {
            int32_t old_qx, old_qy, nx, ny;
            int64_t normal_motion;
            uint16_t wall_angle;
            int16_t reflection;

            /* The previous point identifies the road side. Resolve to that
             * side rather than merely restoring a possibly-overlapping point. */
            nearest_wall_point(old_cx, old_cy, x0, y0, vx, vy, length2,
                               &old_qx, &old_qy);
            nx = old_cx - old_qx;
            ny = old_cy - old_qy;
            if (nx == 0 && ny == 0) {
                /* If the old centre was exactly on the line, use the side
                 * opposite the attempted step. This cannot push it through. */
                nx = -dx; ny = -dy;
                if (nx == 0 && ny == 0) { nx = -vy; ny = vx; }
            }
            normal_motion = (int64_t)(int32_t)(car->x - old_x) * nx +
                            (int64_t)(int32_t)(car->y - old_y) * ny;

            if (normal_motion >= 0) {
                /* Already leaving or travelling along the guardrail. Only
                 * depenetrate; reflecting here is the old wall-sticking bug. */
                release_from_wall(car, qx, qy, nx, ny);
                return 0;
            }

            car->x = old_x; car->y = old_y;
            if (nx * nx + ny * ny < WALL_RELEASE_RADIUS * WALL_RELEASE_RADIUS)
                release_from_wall(car, old_qx, old_qy, nx, ny);

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
            add_effect(game, SH_FX_SPARK, car->x, car->y, 0x40000,
                       0, 0, wall_angle, 0);
            return 1;
        }
    }
    return 0;
}

static void player_tick(SHGame *game, uint16_t pad, int race_started)
{
    SHCar *p = &game->player;
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
    /* cars.c keeps AI on its PATH and does not run the player's sector-wall
     * collision pass. Retaining that split saves four full boundary scans. */
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

static void collide_cars(SHGame *game)
{
    SHCar *player = &game->player;
    unsigned i;
    for (i = 0; i < SH_AI_CARS; ++i) {
        SHCar *other = &game->ai[i];
        int32_t dx = ((int32_t)(player->x - other->x)) >> 18;
        int32_t dy = ((int32_t)(player->y - other->y)) >> 18;
        if (abs32(dx) > 42 || abs32(dy) > 42 || dx * dx + dy * dy >= 42 * 42)
            continue;
        player->x += dx >= 0 ? (2u << 18) : (uint32_t)-(2 << 18);
        player->y += dy >= 0 ? (2u << 18) : (uint32_t)-(2 << 18);
        player->revo = (player->revo * 7) >> 3;
        player->slidspeed = player->v >> 1;
        player->slidcounter = 12;
        player->sliding = 1;
        other->v = (other->v * 7) >> 3;
        ++game->collision_count;
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
    game->player.position = 1;
    for (i = 0; i < SH_AI_CARS; ++i) game->ai[i].position = 1;
    for (i = 0; i < SH_RACERS; ++i)
        for (j = 0; j < SH_RACERS; ++j)
            if (racer_progress(racers[j]) > racer_progress(racers[i])) {
                if (i == 0) ++game->player.position;
                else ++game->ai[i - 1].position;
            }
}

static void update_camera(SHGame *game)
{
    if (game->camera == 1) {
        /* STDCAM_LOW has radius zero and uses the physical body angle. */
        game->camera_angle = game->player.angle;
    } else {
        /* race.c::HandleView(): chase/high cameras lag movement angle by 1/16. */
        int16_t delta = (int16_t)(game->player.movangle - game->camera_angle);
        int16_t step = delta / 16;
        if (step == 0) game->camera_angle = game->player.movangle;
        else game->camera_angle = (uint16_t)(game->camera_angle + step);
    }
}

static void simulation_tick(SHGame *game, uint16_t pad)
{
    unsigned i;
    if (game->mode == SH_MODE_COUNTDOWN) {
        if (game->countdown) --game->countdown;
        if (game->countdown < 70) {
            player_tick(game, pad, 1);
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
    player_tick(game, pad, 1);
    for (i = 0; i < SH_AI_CARS; ++i)
        ai_tick(game, &game->ai[i]);
    collide_cars(game);
    update_effects(game);
    rank_cars(game);
    update_camera(game);
    if (game->player.finished) game->mode = SH_MODE_FINISHED;
}

void sh_game_frame(SHGame *game, uint16_t pad, uint16_t elapsed_vblanks)
{
    uint16_t pressed = pad & (uint16_t)~game->previous_pad;
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
            simulation_tick(game, pad);
        }
        game->sim_accumulator = (uint16_t)accumulator;
    }
}
