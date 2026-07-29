/*
 * Speed Haste 32X race simulation.
 * Derived from game/userctl.c, game/cars.c and game/racemap.c in the
 * original 1995 source release by Javier Arevalo Baeza.
 */
#include "sh_game.h"
#include "sh_assets.h"

#define FP30_ONE (1L << 30)
#define MAX_REVO (1L << 22)

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
static int32_t muldiv(int32_t a, int32_t b, int32_t c)
{
    return c ? (int32_t)(((int64_t)a * b) / c) : 0;
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

static uint16_t vector_angle(int32_t dx, int32_t dy)
{
    uint32_t ax = (uint32_t)abs32(dx), ay = (uint32_t)abs32(dy);
    uint16_t base;
    if ((ax | ay) == 0) return 0;
    if (ax >= ay)
        base = (uint16_t)((uint64_t)ay * 8192u / (ax ? ax : 1));
    else
        base = (uint16_t)(16384u - (uint64_t)ax * 8192u / ay);
    if (dx >= 0 && dy >= 0) return base;
    if (dx < 0 && dy >= 0) return (uint16_t)(32768u - base);
    if (dx < 0) return (uint16_t)(32768u + base);
    return (uint16_t)(0u - base);
}

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
    return assets.tiles[(uint32_t)tile * 4096u +
                        (((y >> 19) & 63u) << 6) + ((x >> 19) & 63u)];
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
    angle = vector_angle(dx, -dy);
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

static void player_tick(uint8_t track, SHCar *p, uint16_t pad, int race_started)
{
    int32_t a, maxva, maxa;
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

    /* userctl.c only enables powersliding for cartype 1 (Stock). */
    if (p->car_type == 1 && p->v > (3 << 19) &&
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
    if (p->v && !p->sliding)
        p->angle = (uint16_t)(p->angle + muldiv(p->va, p->v, 1 << 22));
    p->tirerot = (uint16_t)(p->tirerot + p->v / 512);
    p->tiredir = (uint16_t)(-4 * p->va - (p->v ? muldiv(16 * p->va, p->v, 1 << 22) : 0));
    if (abs32(p->v) > (1 << 9)) p->movangle = vector_angle(dx, -dy);
    else p->movangle = p->angle;
    p->x += (uint32_t)dx;
    p->y += (uint32_t)dy;

    /* racemap.c/userctl.c use palette indices 160..191 as driveable asphalt. */
    {
        uint8_t ground = sh_ground_color(track, p->x, p->y);
        if (ground < 160 || ground >= 192) {
            p->revo -= p->revo >> 7;
            p->z = 0x30000;
        } else p->z = 0x20000;
    }
    (void)braking;
    update_lap(track, p);
}

static void ai_tick(uint8_t track, SHCar *car)
{
    SHPathPoint point;
    int32_t dx, dy, d2;
    int16_t delta;

    if (car->finished) return;
    car->angle = (uint16_t)(car->angle + muldiv(car->av, car->v, 1 << 22));
    car->x += (uint32_t)mul30(car->v, sha_cos30(car->angle));
    car->y += (uint32_t)mul30(car->v, sha_sin30(car->angle));
    update_lap(track, car);

    dx = (int32_t)((car->target_x >> 18) - (car->x >> 18));
    dy = (int32_t)((car->target_y >> 18) - (car->y >> 18));
    d2 = dx * dx + dy * dy;
    if (d2 < (1 << 15) ||
        (d2 < (1 << 19) && abs32((int16_t)(vector_angle(dx, -dy) - car->angle)) > 0x4000)) {
        SHTrackAssets assets;
        sha_get_track(track, &assets);
        car->next_point = (uint8_t)((car->next_point + 1) % assets.path_count);
        set_ai_target(track, car);
        dx = (int32_t)((car->target_x >> 18) - (car->x >> 18));
        dy = (int32_t)((car->target_y >> 18) - (car->y >> 18));
    }
    car->to_angle = vector_angle(dx, -dy);
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
            player_tick(game->selected_track, &game->player, pad, 1);
            for (i = 0; i < SH_AI_CARS; ++i)
                ai_tick(game->selected_track, &game->ai[i]);
        }
        update_camera(game);
        if (!game->countdown) game->mode = SH_MODE_RACE;
        return;
    }
    if (game->mode != SH_MODE_RACE) return;
    ++game->race_ticks;
    player_tick(game->selected_track, &game->player, pad, 1);
    for (i = 0; i < SH_AI_CARS; ++i)
        ai_tick(game->selected_track, &game->ai[i]);
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

    if (pressed & SH_PAD_X) game->camera = (uint8_t)((game->camera + 1) % 3);
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
