.section .text.swtch
.globl swtch
# a0 void *prev_ctx 
# a1 void *after_ctx 
# 交换cpu 和 进程的内核态的上下文
swtch:

#CPU 运行进程 A：
#  A 在用户态执行
#  ← 定时器中断 →
#  A 陷入内核，进入 schedule()
#  调用 swtch(&A->context, &cpu->context)
#  （A 的内核执行流在这里被冻住了，CPU 转去跑调度器循环）
#  
#CPU 运行调度器：
#  调度器继续跑，选中进程 B
#  调用 swtch(&cpu->context, &B->context)
#  （调度器冻住，CPU 开始跑 B 的内核执行流）
#  
#CPU 运行进程 B：
#  B 从上次被冻住的内核位置继续执行（对 B 来说，swtch 刚“返回”）
#  完成剩余工作，sret 回用户态执行
#  过了一会，B 也被定时器中断打断
#  B 陷入内核，进入 schedule()
#  调用 swtch(&B->context, &cpu->context)
#  （B 冻住，CPU 回到调度器）
#CPU 运行调度器：
#  调度器从上次“冻住”的地方继续执行（对调度器来说，swtch 刚“返回”）
#  选中进程 A（或别的）
#  调用 swtch(&cpu->context, &A->context)
#  （调度器冻住，CPU 回到 A）
#CPU 运行进程 A：
#  A 从 swtch 内部“苏醒”，继续执行 schedule() 剩余的代码，sret 回用户态
#  —— 对 A 来说，它只觉得 setTimeout 刚返回，不知道时间过去了多久

    # 保存当前的context寄存器
    sd ra, 0(a0)
    sd sp, 8(a0)
    sd s0,  16(a0)
    sd s1,  24(a0)
    sd s2,  32(a0)
    sd s3,  40(a0)
    sd s4,  48(a0)
    sd s5,  56(a0)
    sd s6,  64(a0)
    sd s7,  72(a0)
    sd s8,  80(a0)
    sd s9,  88(a0)
    sd s10, 96(a0)
    sd s11, 104(a0)
    
    # 加载新的
    ld ra, 0(a1)
    ld sp, 8(a1)
    ld s0,  16(a1)
    ld s1,  24(a1)
    ld s2,  32(a1)
    ld s3,  40(a1)
    ld s4,  48(a1)
    ld s5,  56(a1)
    ld s6,  64(a1)
    ld s7,  72(a1)
    ld s8,  80(a1)
    ld s9,  88(a1)
    ld s10, 96(a1)
    ld s11, 104(a1)

    # 返回
    # 到新的位置
    ret                        
