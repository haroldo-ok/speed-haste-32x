#include <stdint.h>
#include "mars.h"
#include "platform.h"
#include "../sh_render_worker.h"

/* Function called from the SH-2 startup code on the secondary processor. */
void slave(void)
{
#ifdef ENABLE_PWM_AUDIO
    uint32_t phase = 0;
    while ((MARS_SYS_COMM4 & 0x8000u) == 0) { }
    for (;;) {
        uint16_t pitch = MARS_SYS_COMM4 & 0x7FFFu;
        uint16_t sample;
        if (pitch < 8) sample = 522;
        else {
            phase += (uint32_t)pitch << 8;
            sample = (phase & 0x80000000u) ? 650u : 394u;
        }
        while (MARS_PWM_MONO & 0x8000u) { }
        MARS_PWM_MONO = sample;
    }
#elif defined(ENABLE_DUAL_SH2_RENDER)
    /* Render the perspective floor while the master draws the panorama. */
    platform_profile_timer_init();
    MARS_SYS_COMM6 = SH_SLAVE_IDLE;
    for (;;) {
        while (MARS_SYS_COMM6 != SH_SLAVE_DRAW_FLOOR) { }
        sh_render_slave_floor(SH_FLOOR_JOB);
        MARS_SYS_COMM6 = SH_SLAVE_FLOOR_DONE;
        while (MARS_SYS_COMM6 == SH_SLAVE_FLOOR_DONE) { }
    }
#else
    for (;;) __asm__ volatile ("nop");
#endif
}
