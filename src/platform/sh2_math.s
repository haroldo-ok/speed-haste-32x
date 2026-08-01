! SH7604 hardware divider helpers.
! Based on the divider access pattern in D32XR sh2_fixed.s (MIT).

        .section .text
        .align  4
        .global _sh2_idiv
_sh2_idiv:
        mov.l   .Ldiv_unit,r2
        mov.l   r5,@r2              ! DVSR: signed 32-bit divisor
        mov.l   r4,@(4,r2)          ! DVDNT: dividend, starts operation
        rts
        mov.l   @(4,r2),r0          ! quotient after hardware latency

        .align  4
        .global _sh2_muldiv
_sh2_muldiv:
        dmuls.l r4,r5
        sts     mach,r0
        sts     macl,r1
        mov.l   .Ldiv_unit,r2
        mov.l   r6,@r2              ! divisor
        mov.l   r0,@(16,r2)         ! DVDNTH: high signed product
        mov.l   r1,@(20,r2)         ! DVDNTL: low product, start divide
        rts
        mov.l   @(20,r2),r0         ! 32-bit quotient

        .align  4
        .global _sh2_floor_row
_sh2_floor_row:
        mov.l   r8,@-r15
        mov.l   r9,@-r15
        mov.l   r10,@-r15
        mov.l   r11,@-r15
        mov.l   r12,@-r15
        mov.l   r13,@-r15
        mov.l   r14,@-r15

        mov.l   @r4,r5             ! dst row 0
        mov.l   @(4,r4),r6         ! dst row 1
        mov.l   @(8,r4),r7         ! 128x128 map
        mov.l   @(12,r4),r8        ! ROM tile atlas
        mov.l   @(16,r4),r9        ! SDRAM cached tile prefix
        mov.l   @(20,r4),r10       ! selected 256-byte shade row
        mov.l   @(24,r4),r11       ! world X
        mov.l   @(28,r4),r12       ! world Y
        mov.l   @(32,r4),r13       ! X step
        mov     #80,r14            ! 320 pixels / 4x expansion

.Lfloor_loop:
        mov     r11,r0             ! gx = x >> 25
        shlr16  r0
        shlr8   r0
        shlr    r0
        mov     r12,r1             ! gy = y >> 25
        shlr16  r1
        shlr8   r1
        shlr    r1
        shll8   r1                 ! map index = gy * 128 + gx
        shlr    r1
        add     r1,r0
        mov.b   @(r0,r7),r2
        extu.b  r2,r2

        mov.l   @(40,r4),r3        ! choose cached prefix or ROM atlas
        cmp/hs  r3,r2
        bt      .Lfloor_rom
        mov     r9,r1
        bra     .Lfloor_tile_base
        nop
.Lfloor_rom:
        mov     r8,r1
.Lfloor_tile_base:
        mov     r2,r0              ! tile * 4096
        shll8   r0
        shll2   r0
        shll2   r0
        add     r0,r1

        mov     r12,r0             ! ((y >> 19) & 63) * 64
        shlr16  r0
        shlr2   r0
        shlr    r0
        and     #63,r0
        shll2   r0
        shll2   r0
        shll2   r0
        mov     r0,r2
        mov     r11,r0             ! + ((x >> 19) & 63)
        shlr16  r0
        shlr2   r0
        shlr    r0
        and     #63,r0
        add     r2,r0
        mov.b   @(r0,r1),r2
        extu.b  r2,r2
        mov     r2,r0
        mov.b   @(r0,r10),r2       ! palette shade translation
        extu.b  r2,r2

        mov     r2,r0              ! replicate byte to packed 32-bit 4x pixel
        mov     r0,r1
        shll8   r1
        or      r1,r0
        mov     r0,r1
        swap.w  r1,r1
        or      r1,r0
        mov.l   r0,@r5
        mov.l   r0,@r6
        add     #4,r5
        add     #4,r6

        add     r13,r11
        mov.l   @(36,r4),r3        ! Y step
        add     r3,r12
        dt      r14
        bf      .Lfloor_loop
        nop

        mov.l   @r15+,r14
        mov.l   @r15+,r13
        mov.l   @r15+,r12
        mov.l   @r15+,r11
        mov.l   @r15+,r10
        mov.l   @r15+,r9
        mov.l   @r15+,r8
        rts
        nop

        .align  2
.Ldiv_unit:
        .long   0xFFFFFF00
