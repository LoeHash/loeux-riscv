.align 2
.section .text.kernel_trap_vec
.globl kernel_trap_vec

kernel_trap_vec:

        addi sp, sp, -256

        # ra
        sd ra, 0(sp)

        # 8(sp) unused

        # gp / tp
        sd gp, 16(sp)
        sd tp, 24(sp)

        # t0-t2
        sd t0, 32(sp)
        sd t1, 40(sp)
        sd t2, 48(sp)

        # s0-s1
        sd s0, 56(sp)
        sd s1, 64(sp)

        # a0-a7
        sd a0, 72(sp)
        sd a1, 80(sp)
        sd a2, 88(sp)
        sd a3, 96(sp)
        sd a4, 104(sp)
        sd a5, 112(sp)
        sd a6, 120(sp)
        sd a7, 128(sp)

        # s2-s11
        sd s2, 136(sp)
        sd s3, 144(sp)
        sd s4, 152(sp)
        sd s5, 160(sp)
        sd s6, 168(sp)
        sd s7, 176(sp)
        sd s8, 184(sp)
        sd s9, 192(sp)
        sd s10, 200(sp)
        sd s11, 208(sp)

        # t3-t6
        sd t3, 216(sp)
        sd t4, 224(sp)
        sd t5, 232(sp)
        sd t6, 240(sp)

        csrr a0, scause
        csrr a1, sepc
        csrr a2, stval

        call kernel_trap_hanlder

        # ra
        ld ra, 0(sp)

        # gp / tp
        ld gp, 16(sp)
        ld tp, 24(sp)

        # t0-t2
        ld t0, 32(sp)
        ld t1, 40(sp)
        ld t2, 48(sp)

        # s0-s1
        ld s0, 56(sp)
        ld s1, 64(sp)

        # a0-a7
        ld a0, 72(sp)
        ld a1, 80(sp)
        ld a2, 88(sp)
        ld a3, 96(sp)
        ld a4, 104(sp)
        ld a5, 112(sp)
        ld a6, 120(sp)
        ld a7, 128(sp)

        # s2-s11
        ld s2, 136(sp)
        ld s3, 144(sp)
        ld s4, 152(sp)
        ld s5, 160(sp)
        ld s6, 168(sp)
        ld s7, 176(sp)
        ld s8, 184(sp)
        ld s9, 192(sp)
        ld s10, 200(sp)
        ld s11, 208(sp)

        # t3-t6
        ld t3, 216(sp)
        ld t4, 224(sp)
        ld t5, 232(sp)
        ld t6, 240(sp)

        addi sp, sp, 256

        sret