#ifndef _INC_MEMLAYOUT_
#define _INC_MEMLAYOUT_
// Sv39 虚拟地址空间 (64位)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// 0x0000000000000000  ─┐
//                      │  用户空间 (低半区)
// 0x0000003FFFFFFFFF  ─┘  bit 38 = 0，高 25 位全部为 0
//
// 0x0000004000000000  ─┐
//                      │  非法区域（巨大空洞）
// 0xFFFFFF7FFFFFFFFF  ─┘  访问会触发 page fault
//
// 0xFFFFFF8000000000  ─┐  ← 内核空间 (高半区)
//                      │
// 0xFFFFFFFFFFFFFFFF  ─┘  bit 38 = 1，高 25 位全部为 1
// 虚拟地址空间 (Sv39, 38位有效地址)
// ┌────────────────────────────────────────────────────────────────────────────┐
// │ 0x0000_0000_0000  ┌──────────────────────────────────────────────────────┐ │
// │                   │  内核恒等映射区域 (虚拟 = 物理)                          │ │
// │                   │  0x8020_0000 ~ 0x804A_XXXX (约 26 MB)                │ │
// │                   │  权限: R/W/X                                          │ │
// │                   └──────────────────────────────────────────────────────┘ │
// │                                                                            │
// │ 0x0000_0000_1000  ... (可能还有其他恒等映射区)                                 │
// │                                                                            │
// │ 0x3FF8_0000_0000  ┌──────────────────────────────────────────────────────┐ │
// │ (即 MMIO_OFFEST)  │  MMIO 高位映射区 (虚拟 = 物理 + 0x3FF8_0000_0000)       │ │
// │                   │                                                      │ │
// │                   │  ├─ UART    : 0x3FF9_0000_0000                       │ │
// │                   │  │   (物理 0x1000_0000 + 偏移)                        │ │
// │                   │  │                                                   │ │
// │                   │  ├─ VirtIO  : 0x3FF9_0000_1000                       │ │
// │                   │  │   (物理 0x1000_1000 + 偏移)                        │ │
// │                   │  │                                                   │ │
// │                   │  ├─ CLINT   : 0x3FF8_0200_0000                       │ │
// │                   │  │   (物理 0x0200_0000 + 偏移)                        │ │
// │                   │  │                                                   │ │
// │                   │  └─ PLIC    : 0x3FF8_0C00_0000                       │ │
// │                   │      (物理 0x0C00_0000 + 偏移)                        │ │
// │                   └──────────────────────────────────────────────────────┘ │
// │                                                                            │
// │ 0x4000_0000_0000  ┌──────────────────────────────────────────────────────┐ │
// │ (即 MAX_VA)       │  未使用 / 保留区域                                      │ │
// │                   └──────────────────────────────────────────────────────┘ │
// └────────────────────────────────────────────────────────────────────────────┘
#define MAX_VA (uint64_t)(1ULL << (38)) // 使用 sv39能达到的最高的虚拟地址，实际上应该-1

#define KERNEL_START 0x80200000LU
#define MEMORY_START 0x80000000LU

#define STACK_SIZE_PER_CPU 4096 * 2 // 2 pages
#define KSTACK(hartId) (_stack_top - (hartId * STACK_SIZE_PER_CPU))

#define MMIO_OFFEST ((uint64_t)(MAX_VA - (uint64_t)0x08000000UL))
#define MMIO_UART_OFFEST MMIO_OFFEST
#define MMIO_VIRTIO_OFFEST MMIO_OFFEST + UART_PAGE_SIZE * 4096
#define MMIO_CLINT_OFFEST MMIO_OFFEST + UART_PAGE_SIZE * 4096 + VIRTIO_PAGE_SIZE * 4096
#define MMIO_PLIC_OFFEST MMIO_OFFEST + UART_PAGE_SIZE * 4096 + VIRTIO_PAGE_SIZE * 4096 + CLINT_PAGE_SIZE * 4096

#define UART_BASE ((uint64_t)0x10000000LU)
#define VIRTIO_MMIO_BASE ((uint64_t)0x10001000LU)
#define CLINT_BASE ((uint64_t)0x2000000LU)
#define PLIC_BASE ((uint64_t)0xc000000LU)

#define UART_PAGE_SIZE 1
#define VIRTIO_PAGE_SIZE 8
#define CLINT_PAGE_SIZE 16
#define PLIC_PAGE_SIZE 0x600LU

// TRAMPLINE 与 MMIO 之间的保护页
#define GUARD_BT_TRAMP_MMIO TRAMPLINE MMIO_OFFEST - PG_4K_SIZE

// 蹦床页
#define TRAMPLINE GUARD_BT_TRAMP_MMIO - PG_4K_SIZE;

#endif