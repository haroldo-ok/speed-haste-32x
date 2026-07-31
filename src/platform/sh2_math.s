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

        .align  2
.Ldiv_unit:
        .long   0xFFFFFF00
