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
! 2x-unrolled perspective floor kernel. Processes 80 samples per screen row,
! emitting four horizontally-expanded pixels (two packed 32-bit framebuffer
! rows) per sample. step_y and cached_count are loaded once per row instead of
! per sample; r4=rY step, r3=cached tile count persist across the whole row.
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
        mov.l   @(40,r4),r3        ! cached tile count (hoisted)
        mov.l   @(36,r4),r4        ! Y step (hoisted; job ptr no longer needed)
        mov     #40,r14            ! 80 samples / 2 per iteration

.Lfloor_loop:
        ! ===================== sample 0 =====================
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
        mov.b   @(r0,r7),r2        ! tile index in r2
        extu.b  r2,r2

        cmp/hs  r3,r2              ! choose cached prefix or ROM atlas
        bt      .Lf0_rom
        mov     r9,r1              ! base = cached prefix
        bra     .Lf0_base
        nop
.Lf0_rom:
        mov     r8,r1              ! base = ROM atlas
.Lf0_base:
        mov     r2,r0              ! tile * 4096
        shll8   r0
        shll2   r0
        shll2   r0
        add     r0,r1              ! r1 = tile base address

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
        add     r2,r0              ! texel index in r0
        mov.b   @(r0,r1),r2        ! index r0, base r1
        extu.b  r2,r2              ! texel color byte in r2
        mov     r2,r0              ! texel color byte in r0
        shll2   r0                ! packed table is 4-byte entries -> color*4
        mov.l   @(r0,r10),r0       ! packed shade (r10 = packed shade row)
        mov.l   r0,@r5
        mov.l   r0,@r6
        add     #4,r5
        add     #4,r6
        add     r13,r11
        add     r4,r12             ! r4 = step_y (hoisted)

        ! ===================== sample 1 =====================
        mov     r11,r0
        shlr16  r0
        shlr8   r0
        shlr    r0
        mov     r12,r1
        shlr16  r1
        shlr8   r1
        shlr    r1
        shll8   r1
        shlr    r1
        add     r1,r0
        mov.b   @(r0,r7),r2
        extu.b  r2,r2              ! tile index in r2

        cmp/hs  r3,r2
        bt      .Lf1_rom
        mov     r9,r1
        bra     .Lf1_base
        nop
.Lf1_rom:
        mov     r8,r1
.Lf1_base:
        mov     r2,r0              ! tile * 4096
        shll8   r0
        shll2   r0
        shll2   r0
        add     r0,r1              ! r1 = tile base address

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
        add     r2,r0              ! texel index in r0
        mov.b   @(r0,r1),r2
        extu.b  r2,r2              ! texel color byte in r2
        mov     r2,r0              ! texel color byte in r0
        shll2   r0                ! packed table is 4-byte entries -> color*4
        mov.l   @(r0,r10),r0       ! packed shade (r10 = packed shade row)
        mov.l   r0,@r5
        mov.l   r0,@r6
        add     #4,r5
        add     #4,r6
        add     r13,r11
        add     r4,r12             ! r4 = step_y (hoisted)

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

        .align  4
        .global _sh2_floor_row_far
! 8-pixel-column variant for the perspective-compressed far floor rows. 40
! samples per row, each emitted as two packed words. step_x/step_y already
! encode the 8-px world step; pos_x/pos_y start at the first 8-px column.
_sh2_floor_row_far:
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
        mov.l   @(20,r4),r10       ! selected shade row
        mov.l   @(24,r4),r11       ! world X
        mov.l   @(28,r4),r12       ! world Y
        mov.l   @(32,r4),r13       ! X step (already 8-px)
        mov.l   @(40,r4),r3        ! cached tile count
        mov.l   @(36,r4),r4        ! Y step (hoisted)
        mov     #40,r14

.Lffar_loop:
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
        extu.b  r2,r2              ! tile index

        cmp/hs  r3,r2
        bt      .Lff_rom
        mov     r9,r1
        bra     .Lff_base
        nop
.Lff_rom:
        mov     r8,r1
.Lff_base:
        mov     r2,r0              ! tile * 4096
        shll8   r0
        shll2   r0
        shll2   r0
        add     r0,r1              ! tile base

        mov     r12,r0             ! ((y>>19)&63)*64
        shlr16  r0
        shlr2   r0
        shlr    r0
        and     #63,r0
        shll2   r0
        shll2   r0
        shll2   r0
        mov     r0,r2
        mov     r11,r0             ! + (x>>19)&63
        shlr16  r0
        shlr2   r0
        shlr    r0
        and     #63,r0
        add     r2,r0              ! texel index
        mov.b   @(r0,r1),r2
        extu.b  r2,r2              ! texel color byte in r2
        mov     r2,r0              ! texel color byte in r0
        shll2   r0                ! packed table is 4-byte entries -> color*4
        mov.l   @(r0,r10),r0       ! packed shade (r10 = packed shade row)

        mov.l   r0,@r5             ! two packed words per sample (8 px)
        mov.l   r0,@(4,r5)
        add     #8,r5
        mov.l   r0,@r6
        mov.l   r0,@(4,r6)
        add     #8,r6
        add     r13,r11
        add     r4,r12

        dt      r14
        bf      .Lffar_loop
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
