#ifndef SPEED_HASTE_PLATFORM_H
#define SPEED_HASTE_PLATFORM_H

#include <stdint.h>

#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 224

void platform_init(void);
volatile uint8_t *platform_back_buffer(void);
void platform_flip(void);
uint16_t platform_read_pad(void);
uint16_t platform_read_pad2(void);
int platform_pad2_present(void);
void platform_set_engine_pitch(uint16_t pitch);
void platform_set_vga_palette(const uint8_t *vga_rgb);
uint32_t platform_vblank_count(void);
void platform_profile_timer_init(void);
uint8_t platform_profile_ticks(void);

#endif
