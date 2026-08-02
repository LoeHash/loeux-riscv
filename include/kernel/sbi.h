// sbi.h
#ifndef __SBI_H__
#define __SBI_H__
#include <type.h>

// 成功
#define SBI_SUCCESS 0
// 失败
#define SBI_ERR_FAILED -1
// 不支持操作
#define SBI_ERR_NOT_SUPPORTED -2
// 非法参数
#define SBI_ERR_INVALID_PARAM -3
// 拒绝
#define SBI_ERR_DENIED -4
// 非法地址
#define SBI_ERR_INVALID_ADDRESS -5
// (资源) 已可用
#define SBI_ERR_ALREADY_AVAILABLE -6
// (操作) 已启动
#define SBI_ERR_ALREADY_STARTED -7
// (操作) 已停止
#define SBI_ERR_ALREADY_STOPPED -8

#define SBI_FID_ZERO 0

/* SBI Extension IDs */
#define SBI_EXT_0_1_SET_TIMER 0x0
#define SBI_EXT_0_1_CONSOLE_PUTCHAR 0x1
#define SBI_EXT_0_1_CONSOLE_GETCHAR 0x2
#define SBI_EXT_0_1_CLEAR_IPI 0x3
#define SBI_EXT_0_1_SEND_IPI 0x4
#define SBI_EXT_0_1_REMOTE_FENCE_I 0x5
#define SBI_EXT_0_1_REMOTE_SFENCE_VMA 0x6
#define SBI_EXT_0_1_REMOTE_SFENCE_VMA_ASID 0x7
#define SBI_EXT_0_1_SHUTDOWN 0x8
#define SBI_EXT_BASE 0x10
#define SBI_EXT_TIME 0x54494D45
#define SBI_EXT_IPI 0x735049
#define SBI_EXT_RFENCE 0x52464E43
#define SBI_EXT_HSM 0x48534D
#define SBI_EXT_SRST 0x53525354
#define SBI_EXT_PMU 0x504D55
#define SBI_EXT_DBCN 0x4442434E
#define SBI_EXT_SUSP 0x53555350
#define SBI_EXT_CPPC 0x43505043
#define SBI_EXT_DBTR 0x44425452
#define SBI_EXT_SSE 0x535345
#define SBI_EXT_FWFT 0x46574654
#define SBI_EXT_MPXY 0x4D505859

/* SBI function IDs for BASE extension*/
#define SBI_EXT_BASE_GET_SPEC_VERSION 0x0
#define SBI_EXT_BASE_GET_IMP_ID 0x1
#define SBI_EXT_BASE_GET_IMP_VERSION 0x2
#define SBI_EXT_BASE_PROBE_EXT 0x3
#define SBI_EXT_BASE_GET_MVENDORID 0x4
#define SBI_EXT_BASE_GET_MARCHID 0x5
#define SBI_EXT_BASE_GET_MIMPID 0x6

/* SBI function IDs for TIME extension*/
#define SBI_EXT_TIME_SET_TIMER 0x0

/* SBI function IDs for IPI extension*/
#define SBI_EXT_IPI_SEND_IPI 0x0

/* SBI function IDs for RFENCE extension*/
#define SBI_EXT_RFENCE_REMOTE_FENCE_I 0x0
#define SBI_EXT_RFENCE_REMOTE_SFENCE_VMA 0x1
#define SBI_EXT_RFENCE_REMOTE_SFENCE_VMA_ASID 0x2
#define SBI_EXT_RFENCE_REMOTE_HFENCE_GVMA_VMID 0x3
#define SBI_EXT_RFENCE_REMOTE_HFENCE_GVMA 0x4
#define SBI_EXT_RFENCE_REMOTE_HFENCE_VVMA_ASID 0x5
#define SBI_EXT_RFENCE_REMOTE_HFENCE_VVMA 0x6

/* SBI function IDs for HSM extension */
#define SBI_EXT_HSM_HART_START 0x0
#define SBI_EXT_HSM_HART_STOP 0x1
#define SBI_EXT_HSM_HART_GET_STATUS 0x2
#define SBI_EXT_HSM_HART_SUSPEND 0x3

#define SBI_HSM_STATE_STARTED 0x0
#define SBI_HSM_STATE_STOPPED 0x1
#define SBI_HSM_STATE_START_PENDING 0x2
#define SBI_HSM_STATE_STOP_PENDING 0x3
#define SBI_HSM_STATE_SUSPENDED 0x4
#define SBI_HSM_STATE_SUSPEND_PENDING 0x5
#define SBI_HSM_STATE_RESUME_PENDING 0x6

#define SBI_HSM_SUSP_BASE_MASK 0x7fffffff
#define SBI_HSM_SUSP_NON_RET_BIT 0x80000000
#define SBI_HSM_SUSP_PLAT_BASE 0x10000000

#define SBI_HSM_SUSPEND_RET_DEFAULT 0x00000000
#define SBI_HSM_SUSPEND_RET_PLATFORM SBI_HSM_SUSP_PLAT_BASE
#define SBI_HSM_SUSPEND_RET_LAST SBI_HSM_SUSP_BASE_MASK
#define SBI_HSM_SUSPEND_NON_RET_DEFAULT SBI_HSM_SUSP_NON_RET_BIT
#define SBI_HSM_SUSPEND_NON_RET_PLATFORM (SBI_HSM_SUSP_NON_RET_BIT | \
                                          SBI_HSM_SUSP_PLAT_BASE)
#define SBI_HSM_SUSPEND_NON_RET_LAST (SBI_HSM_SUSP_NON_RET_BIT | \
                                      SBI_HSM_SUSP_BASE_MASK)

/* SBI function IDs for SRST extension */
#define SBI_EXT_SRST_RESET 0x0

#define SBI_SRST_RESET_TYPE_SHUTDOWN 0x0
#define SBI_SRST_RESET_TYPE_COLD_REBOOT 0x1
#define SBI_SRST_RESET_TYPE_WARM_REBOOT 0x2
#define SBI_SRST_RESET_TYPE_LAST SBI_SRST_RESET_TYPE_WARM_REBOOT

#define SBI_SRST_RESET_REASON_NONE 0x0
#define SBI_SRST_RESET_REASON_SYSFAIL 0x1

/* SBI function IDs for PMU extension */
#define SBI_EXT_PMU_NUM_COUNTERS 0x0
#define SBI_EXT_PMU_COUNTER_GET_INFO 0x1
#define SBI_EXT_PMU_COUNTER_CFG_MATCH 0x2
#define SBI_EXT_PMU_COUNTER_START 0x3
#define SBI_EXT_PMU_COUNTER_STOP 0x4
#define SBI_EXT_PMU_COUNTER_FW_READ 0x5
#define SBI_EXT_PMU_COUNTER_FW_READ_HI 0x6
#define SBI_EXT_PMU_SNAPSHOT_SET_SHMEM 0x7
#define SBI_EXT_PMU_EVENT_GET_INFO 0x8

/* SBI function IDs for DBTR extension */
#define SBI_EXT_DBTR_NUM_TRIGGERS 0x0
#define SBI_EXT_DBTR_SETUP_SHMEM 0x1
#define SBI_EXT_DBTR_TRIGGER_READ 0x2
#define SBI_EXT_DBTR_TRIGGER_INSTALL 0x3
#define SBI_EXT_DBTR_TRIGGER_UPDATE 0x4
#define SBI_EXT_DBTR_TRIGGER_UNINSTALL 0x5
#define SBI_EXT_DBTR_TRIGGER_ENABLE 0x6
#define SBI_EXT_DBTR_TRIGGER_DISABLE 0x7

/* SBI function IDs for FW feature extension */
#define SBI_EXT_FWFT_SET 0x0
#define SBI_EXT_FWFT_GET 0x1

// 调用sbi服务叶子函数
static inline unsigned long sbi_ecall(
    unsigned long eid, unsigned long fid,
    unsigned long arg0, unsigned long arg1,
    unsigned long arg2, unsigned long arg3,
    unsigned long arg4, unsigned long arg5)
{
        register unsigned long a0 asm("a0") = arg0;
        register unsigned long a1 asm("a1") = arg1;
        register unsigned long a2 asm("a2") = arg2;
        register unsigned long a3 asm("a3") = arg3;
        register unsigned long a4 asm("a4") = arg4;
        register unsigned long a5 asm("a5") = arg5;
        register unsigned long fun_id asm("a6") = fid;
        register unsigned long ext_id asm("a7") = eid;

        __asm__ volatile(
            "ecall"
            : "=r"(a0), "=r"(a1)
            : "0"(a0), "1"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(fun_id), "r"(ext_id) // 表示哪些寄存器会作为输入寄存器, 这样编译器不会改变它们的值
            : "memory");

        return a0;
}

static inline unsigned long sbi_set_timer(uint64_t abs_time)
{
        return sbi_ecall(SBI_EXT_TIME,
                         SBI_EXT_TIME_SET_TIMER,
                         abs_time,
                         0, 0, 0, 0, 0);
}

static inline unsigned long sbi_hart_start(unsigned long hartid,
                                           unsigned long start_addr,
                                           unsigned long opaque)
{
        return sbi_ecall(SBI_EXT_HSM,
                         SBI_EXT_HSM_HART_START,
                         hartid,
                         start_addr,
                         opaque, 0, 0, 0);
}

// putchar
static inline unsigned long sbi_putchar(char c)
{
        return sbi_ecall(SBI_EXT_0_1_CONSOLE_PUTCHAR, SBI_FID_ZERO, (unsigned long)c, 0, 0, 0, 0, 0);
}

#endif