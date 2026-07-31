.section .text._entry64
.globl _entry64
_entry64:
        # 保存设备指针和核心id
        mv s0, a0
        mv s1, a1

        # 设置栈指针
        # 根据不同的核心id, 对 栈进行划分
        # 使用 _stack_
        li t1, 4096 * 4

        addi a0, a0, 1
        
        mul t0, a0, t1
        la t1, _stack_
        add sp, t1, t0

        # bss 段清零
        la t0, _bss_start
        la t1, _bss_end
fill_zero:
        sd zero, 0(t0)
        addi t0, t0, 8
        bltu t0, t1, fill_zero

jmp_to_kernel:
        # 恢复
        mv a0, s0
        mv a1, s1
        call kstart
dead:
        j dead  # jump to dead