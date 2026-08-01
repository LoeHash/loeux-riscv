
.section .text._trampoline_jump
.globl _trampoline_jump
_trampoline_jump:
        # 在这里我们认为: trapframe已经被映射到了一个虚拟地址
        # 且用户有 RX, 同时当前页表是用户页表，且我们是 S-mode
        # 因此，我们只需要:
        csrw sscratch, a0 # 把 a0 的值保存
        li a0, 0x3F7FFFC000 
        sd ra, 40(a0) 
        sd sp, 48(a0) 
        sd gp, 56(a0) 
        sd tp, 64(a0) 
        sd t0, 72(a0) 
        sd t1, 80(a0) 
        sd t2, 88(a0) 
        sd s0, 96(a0) 
        sd s1, 104(a0) 
        # sd a0, 112(a0) 
        sd a1, 120(a0) 
        sd a2, 128(a0) 
        sd a3, 136(a0) 
        sd a4, 144(a0) 
        sd a5, 152(a0) 
        sd a6, 160(a0) 
        sd a7, 168(a0) 
        sd s2, 176(a0) 
        sd s3, 184(a0) 
        sd s4, 192(a0) 
        sd s5, 200(a0) 
        sd s6, 208(a0) 
        sd s7, 216(a0) 
        sd s8, 224(a0) 
        sd s9, 232(a0) 
        sd s10, 240(a0) 
        sd s11, 248(a0) 
        sd t3, 256(a0) 
        sd t4, 264(a0) 
        sd t5, 272(a0) 
        sd t6, 280(a0)

        # 把a0 保存
        csrr t0, sscratch
        sd t0, 112(a0)

        # 保存sepc
        csrr t0, sepc
        sd t0, 32(a0)

        # 保存hartid
        sd tp, 24(a0)
        
        # 设置栈指针
        # 注意这个栈指针必须要在satp设置为内核页表
        # 后才能使用!
        # 我们认为进程在退出内核态前已经设置好了内核栈指针
        ld t0, 8(a0)
        mv sp, t0

        # 保存user_trap_hanlder
        # 同样我们也认为已经设置好了
        ld t0, 16(a0)

        # 加载内核页表
        ld t1, 0(a0)

        # 切换页表
        sfence.vma zero, zero
        csrw satp, t1
        sfence.vma zero, zero
        
        # 这里就进入了内核页表了
        # 由于我们在内核建立蹦床页的映射
        # 因此cpu取址直接可以得到
        # 现在：内核栈指针设置了， 页表也切换了
        # 直接进入 c语言
        # 需要先设置几个参数
        csrr a0, scause
        csrr a1, sepc
        csrr a2, stval
         
        jalr t0
        
_trampoline_ret:
        # 回到这里 a0寄存器保留了
        # 我们的用户satp
        # 但现在我们还是在使用内核页表
        # 进行切换
        sfence.vma zero, zero
        csrw satp, a0
        sfence.vma zero, zero

        li a0, 0x3F7FFFC000
        ld ra, 40(a0) 
        ld sp, 48(a0) 
        ld gp, 56(a0) 
        ld tp, 64(a0) 
        ld t0, 72(a0) 
        ld t1, 80(a0) 
        ld t2, 88(a0) 
        ld s0, 96(a0) 
        ld s1, 104(a0) 
        # sd a0, 112(a0) 
        ld a1, 120(a0) 
        ld a2, 128(a0) 
        ld a3, 136(a0) 
        ld a4, 144(a0) 
        ld a5, 152(a0) 
        ld a6, 160(a0) 
        ld a7, 168(a0) 
        ld s2, 176(a0) 
        ld s3, 184(a0) 
        ld s4, 192(a0) 
        ld s5, 200(a0) 
        ld s6, 208(a0) 
        ld s7, 216(a0) 
        ld s8, 224(a0) 
        ld s9, 232(a0) 
        ld s10, 240(a0) 
        ld s11, 248(a0) 
        ld t3, 256(a0) 
        ld t4, 264(a0) 
        ld t5, 272(a0) 
        ld t6, 280(a0)

        csrw sscratch, t0  
        mv t0, a0
        ld a0, 112(t0)
        csrr t0, sscratch

        #ok 准备返回
        sret