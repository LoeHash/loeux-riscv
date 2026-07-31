.section .text._sec_entry64
.globl _sec_entry64
_sec_entry64:
        
        mv s0, a0  # hart id
        mv s1, a1  # 补充数据地址

        # 设置栈指针
        # 根据不同的核心id, 对 栈进行划分
        # 使用 _stack_
        li t1, 4096 * 4

        addi a0, a0, 1
        
        mul t0, a0, t1
        la t1, _stack_
        add sp, t1, t0

jmp_to_secondary_kernel:
        # 恢复
        mv a0, s0
        mv a1, s1
        call secondary_start
dead:
        j dead  # jump to dead