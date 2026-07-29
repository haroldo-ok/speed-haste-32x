#ifndef SH_RENDER_H
#define SH_RENDER_H
#include <stdint.h>
#include "sh_game.h"
void sh_render_frame(volatile uint8_t *framebuffer, const SHGame *game);
#endif
