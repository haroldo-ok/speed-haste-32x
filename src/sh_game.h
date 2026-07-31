#ifndef SH_GAME_H
#define SH_GAME_H

#include <stdint.h>

#define SH_AI_CARS 4
#define SH_RACERS (SH_AI_CARS + 1)
#define SH_LAPS 3
#define SH_EFFECTS 16

enum {
    SH_FX_NONE,
    SH_FX_SPARK,
    SH_FX_SMOKE,
    SH_FX_SKID
};

typedef struct SHEffect {
    uint32_t x, y, z;
    int32_t dx, dy;
    uint16_t age, life, angle;
    uint8_t type, variant;
} SHEffect;

enum {
    SH_MODE_TITLE,
    SH_MODE_MENU,
    SH_MODE_COUNTDOWN,
    SH_MODE_RACE,
    SH_MODE_PAUSED,
    SH_MODE_FINISHED
};

enum {
    SH_MENU_MAIN,
    SH_MENU_CIRCUIT,
    SH_MENU_CLASS,
    SH_MENU_CAR
};

enum {
    SH_PAD_UP = 0x0001,
    SH_PAD_DOWN = 0x0002,
    SH_PAD_LEFT = 0x0004,
    SH_PAD_RIGHT = 0x0008,
    SH_PAD_B = 0x0010,
    SH_PAD_C = 0x0020,
    SH_PAD_A = 0x0040,
    SH_PAD_START = 0x0080,
    SH_PAD_Z = 0x0100,
    SH_PAD_Y = 0x0200,
    SH_PAD_X = 0x0400
};

typedef struct SHCar {
    uint32_t x, y, z;
    uint16_t angle, movangle, ma;
    int16_t va;
    int32_t v, revo, slidspeed;
    int16_t slidva;
    int16_t slidcounter;
    uint16_t tirerot, tiredir;
    uint8_t gear, automatic, model, car_type;
    uint8_t position, nlap, npoint, finished;
    uint8_t sliding, reserved;
    uint16_t best_lap, max_speed;
    uint32_t lap_time, total_time;

    /* Computer driver state, mirroring cars.c. */
    uint8_t next_point, car_speed;
    int16_t av;
    uint16_t to_angle;
    int32_t to_speed;
    uint32_t target_x, target_y;
} SHCar;

typedef struct SHGame {
    uint8_t mode, camera, hud, menu_page;
    uint8_t selected_car, selected_track, car_type, menu_latched;
    uint8_t menu_release, reserved;
    uint16_t previous_pad;
    uint16_t camera_angle;
    uint16_t countdown;
    uint16_t sim_accumulator;
    uint16_t collision_count;
    uint16_t wall_collision_count;
    uint32_t frame;
    uint32_t race_ticks;
    SHCar player;
    SHCar ai[SH_AI_CARS];
    SHEffect effects[SH_EFFECTS];
} SHGame;

void sh_game_init(SHGame *game);
void sh_game_frame(SHGame *game, uint16_t pad, uint16_t elapsed_vblanks);
uint8_t sh_ground_color(uint8_t track, uint32_t x, uint32_t y);

#endif
