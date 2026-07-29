#ifndef SPEED_HASTE_MARS_H
#define SPEED_HASTE_MARS_H

#include <stdint.h>

#define MARS_CRAM           (*(volatile uint16_t *)0x20004200)
#define MARS_FRAMEBUFFER    (*(volatile uint16_t *)0x24000000)

#define MARS_SYS_INTMSK     (*(volatile uint16_t *)0x20004000)
#define MARS_SYS_COMM0      (*(volatile uint16_t *)0x20004020)
#define MARS_SYS_COMM2      (*(volatile uint16_t *)0x20004022)
#define MARS_SYS_COMM4      (*(volatile uint16_t *)0x20004024)
#define MARS_SYS_COMM6      (*(volatile uint16_t *)0x20004026)
#define MARS_SYS_COMM8      (*(volatile uint16_t *)0x20004028)
#define MARS_SYS_COMM10     (*(volatile uint16_t *)0x2000402A)
#define MARS_SYS_COMM12     (*(volatile uint32_t *)0x2000402C)

#define MARS_PWM_CTRL       (*(volatile uint16_t *)0x20004030)
#define MARS_PWM_CYCLE      (*(volatile uint16_t *)0x20004032)
#define MARS_PWM_MONO       (*(volatile uint16_t *)0x20004038)

#define MARS_VDP_DISPMODE   (*(volatile uint16_t *)0x20004100)
#define MARS_VDP_FBCTL      (*(volatile uint16_t *)0x2000410A)

#define MARS_SH2_ACCESS_VDP 0x8000u
#define MARS_VDP_PRIO_32X   0x0080u
#define MARS_224_LINES      0x0000u
#define MARS_VDP_MODE_256   0x0001u
#define MARS_VDP_FS         0x0001u

#define SEGA_CTRL_UP        0x0001u
#define SEGA_CTRL_DOWN      0x0002u
#define SEGA_CTRL_LEFT      0x0004u
#define SEGA_CTRL_RIGHT     0x0008u
#define SEGA_CTRL_B         0x0010u
#define SEGA_CTRL_C         0x0020u
#define SEGA_CTRL_A         0x0040u
#define SEGA_CTRL_START     0x0080u
#define SEGA_CTRL_Z         0x0100u
#define SEGA_CTRL_Y         0x0200u
#define SEGA_CTRL_X         0x0400u
#define SEGA_CTRL_MODE      0x0800u
#define SEGA_CTRL_TYPE      0xF000u
#define SEGA_CTRL_NONE      0xF000u

#define MARS_RGB(r, g, b) ((uint16_t)(((r) & 31) | (((g) & 31) << 5) | (((b) & 31) << 10)))

#endif
