#include <stdint.h>
#include "sh_game.h"
#include "sh_render.h"
#include "platform/platform.h"

int main(void)
{
    SHGame game;
    uint32_t last_vblank;
    platform_init();
    sh_game_init(&game);
    last_vblank = platform_vblank_count();
    for (;;) {
        uint32_t now = platform_vblank_count();
        uint16_t elapsed = (uint16_t)(now - last_vblank);
        uint16_t pad = platform_read_pad();
        uint16_t pad2 = platform_read_pad2();
        last_vblank = now;
        sh_game_frame(&game, pad, pad2, elapsed);
        sh_render_frame(platform_back_buffer(), &game);
        platform_flip();
    }
    return 0;
}
