#include "platform.h"
#include "mars.h"

/* SH7604 watchdog in interval-timer mode. CKS=7 clocks at Sclk/8192, giving
 * useful sub-millisecond render profiling without overflowing during a normal
 * phase. PicoDrive updates WTCNT on reads, unlike its currently inert FRT. */
#define SH2_WTCSR_WTCNT (*(volatile uint16_t *)0xFFFFFE80u)
#define SH2_WTCNT       (*(volatile uint8_t *)0xFFFFFE81u)

static uint16_t current_fb;

static void init_accessible_framebuffer(void)
{
    volatile uint16_t *fb = &MARS_FRAMEBUFFER;
    int i;

    for (i = 0; i < SCREEN_HEIGHT; ++i)
        fb[i] = (uint16_t)(0x100 + i * (SCREEN_WIDTH / 2));

    for (i = 0x100; i < 0x10000; ++i)
        fb[i] = 0;
}

static void flip_and_wait(void)
{
    MARS_VDP_FBCTL = current_fb ^ 1u;
    while ((MARS_VDP_FBCTL & MARS_VDP_FS) == current_fb) { }
    current_fb ^= 1u;
}

static void init_palette(void)
{
    volatile uint16_t *pal = &MARS_CRAM;
    int i;

    for (i = 0; i < 256; ++i)
        pal[i] = MARS_RGB(0, 0, 0);

    pal[0]  = MARS_RGB(0, 0, 0);       /* transparent/black */
    pal[1]  = MARS_RGB(4, 10, 22);     /* upper sky */
    pal[2]  = MARS_RGB(11, 20, 31);    /* lower sky */
    pal[3]  = MARS_RGB(3, 12, 5);      /* grass dark */
    pal[4]  = MARS_RGB(5, 18, 7);      /* grass light */
    pal[5]  = MARS_RGB(12, 12, 13);    /* road */
    pal[6]  = MARS_RGB(9, 9, 10);      /* road dark */
    pal[7]  = MARS_RGB(31, 31, 31);    /* white */
    pal[8]  = MARS_RGB(31, 3, 3);      /* red */
    pal[9]  = MARS_RGB(31, 25, 2);     /* yellow */
    pal[10] = MARS_RGB(2, 25, 31);     /* cyan */
    pal[11] = MARS_RGB(31, 10, 2);     /* orange */
    pal[12] = MARS_RGB(8, 8, 9);       /* panel */
    pal[13] = MARS_RGB(18, 18, 20);    /* panel edge */
    pal[14] = MARS_RGB(2, 31, 10);     /* green */
    pal[15] = MARS_RGB(19, 4, 25);     /* purple */
    pal[16] = MARS_RGB(24, 24, 27);    /* road line */
    pal[17] = MARS_RGB(16, 8, 2);      /* dirt */
    pal[18] = MARS_RGB(31, 17, 2);     /* player body */
    pal[19] = MARS_RGB(4, 4, 5);       /* tires */
    pal[20] = MARS_RGB(19, 27, 31);    /* glass */

    /* Stable, unique colors used by emulator state probes. */
    pal[21] = MARS_RGB(31, 0, 25);     /* title */
    pal[22] = MARS_RGB(31, 31, 0);     /* countdown */
    pal[23] = MARS_RGB(0, 31, 31);     /* race */
    pal[24] = MARS_RGB(31, 12, 0);     /* paused */
    pal[25] = MARS_RGB(0, 31, 0);      /* finished */
}

void platform_profile_timer_init(void)
{
    SH2_WTCSR_WTCNT = 0x5A00u; /* WTCNT = 0 */
    SH2_WTCSR_WTCNT = 0xA527u; /* interval timer, enabled, Sclk/8192 */
}

void platform_init(void)
{
    current_fb = 0;
    while ((MARS_SYS_INTMSK & MARS_SH2_ACCESS_VDP) == 0) { }
    platform_profile_timer_init();

    MARS_VDP_DISPMODE = MARS_VDP_PRIO_32X | MARS_224_LINES | MARS_VDP_MODE_256;
    init_palette();

    flip_and_wait();
    init_accessible_framebuffer();
    flip_and_wait();
    init_accessible_framebuffer();

#ifdef ENABLE_PWM_AUDIO
    /* 22.05 kHz PWM. Slave SH-2 feeds the FIFO after COMM4 is armed. */
    MARS_PWM_MONO = 1;
    MARS_PWM_MONO = 1;
    MARS_PWM_MONO = 1;
    MARS_PWM_CYCLE = 1045;
    MARS_PWM_CTRL = 0x0185;
    MARS_SYS_COMM4 = 0x8001;
#else
    /*
     * Compatibility default: older ARM PicoDrive dynarecs can stall while a
     * slave SH-2 continuously polls the PWM FIFO. Leave PWM off and park the
     * slave unless AUDIO=1 is explicitly requested at build time.
     */
    MARS_SYS_COMM4 = 0;
#endif
}

volatile uint8_t *platform_back_buffer(void)
{
    return ((volatile uint8_t *)&MARS_FRAMEBUFFER) + 0x200;
}

void platform_flip(void)
{
    flip_and_wait();
}

uint16_t platform_read_pad(void)
{
    uint16_t pad = MARS_SYS_COMM8;
    if ((pad & SEGA_CTRL_TYPE) == SEGA_CTRL_NONE)
        pad = 0;
    return pad & 0x0FFFu;
}

uint16_t platform_read_pad2(void)
{
    /* The 68000 stores controller 2 at 0xA1512A, which the 32X exposes to the
     * SH-2s at MARS_SYS_COMM10. */
    uint16_t pad = MARS_SYS_COMM10;
    if ((pad & SEGA_CTRL_TYPE) == SEGA_CTRL_NONE)
        pad = 0;
    return pad & 0x0FFFu;
}

uint32_t platform_vblank_count(void)
{
    return MARS_SYS_COMM12;
}

uint8_t platform_profile_ticks(void)
{
    return SH2_WTCNT;
}

void platform_set_vga_palette(const uint8_t *vga_rgb)
{
    volatile uint16_t *pal = &MARS_CRAM;
    int i;
    for (i = 0; i < 256; ++i) {
        uint16_t r = (uint16_t)(vga_rgb[i * 3 + 0] >> 1);
        uint16_t g = (uint16_t)(vga_rgb[i * 3 + 1] >> 1);
        uint16_t b = (uint16_t)(vga_rgb[i * 3 + 2] >> 1);
        pal[i] = MARS_RGB(r, g, b);
    }
}

void platform_set_engine_pitch(uint16_t pitch)
{
#ifdef ENABLE_PWM_AUDIO
    if (pitch > 0x7FFFu)
        pitch = 0x7FFFu;
    MARS_SYS_COMM4 = (uint16_t)(0x8000u | pitch);
#else
    (void)pitch;
#endif
}
