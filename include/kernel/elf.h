#ifndef _INC_ELF_
#define _INC_ELF_
#include <type.h>

#define ELF_MAGIC 0x464C457FU
#define EI_MAG0_OFFSET 0
#define EI_MAG1_OFFSET 1
#define EI_MAG2_OFFSET 2
#define EI_MAG3_OFFSET 3
#define EI_CLASS_OFFSET 4
#define EI_DATA_OFFSET 5
#define EI_VERSION_OFFSET 6

#define EI_MAG0_VAL 0x7f
#define EI_MAG1_VAL 0x45
#define EI_MAG2_VAL 0x4c
#define EI_MAG3_VAL 0x46
#define EI_CLASS_VAL 2
#define EI_DATA_VAL 1
#define EI_VERSION_VAL 1
#define E_TYPE 2
#define E_MACHINE 243
#define E_VERSION 1
#define E_EHSIZE 64
#define E_PHENTSIZE 56

// pt
#define PT_NULL 0    /* 空条目，忽略 */
#define PT_LOAD 1    /* 可加载段（你要的） */
#define PT_DYNAMIC 2 /* 动态链接信息 */
#define PT_INTERP 3  /* 动态链接器路径 */
#define PT_NOTE 4    /* 辅助信息 */
#define PT_SHLIB 5   /* 保留 */
#define PT_PHDR 6    /* program header 表自身 */
#define PT_TLS 7     /* 线程局部存储模板 */
#define PT_RISCV_ATTRIBUT 0x70000003

// pt flags
#define PF_X 1
#define PF_W 2
#define PF_R 4

// ELF 头，共 64 字节
struct elf64_ehdr
{
        uint8_t e_ident[16];  // 魔数在 [0..3]
        uint16_t e_type;      // 偏移 16
        uint16_t e_machine;   // 偏移 18
        uint32_t e_version;   // 偏移 20
        uint64_t e_entry;     // 偏移 32  ← 入口点
        uint64_t e_phoff;     // 偏移 40  ← phdr 表在文件里的偏移
        uint64_t e_shoff;     // 偏移 48
        uint32_t e_flags;     // 偏移 56
        uint16_t e_ehsize;    // 偏移 60
        uint16_t e_phentsize; // 偏移 62  ← 每个 phdr 多大（应该是 56）
        uint16_t e_phnum;     // 偏移 64  ← 有几个 phdr
        uint16_t e_shentsize;
        uint16_t e_shnum;
        uint16_t e_shstrndx;
} __attribute__((packed));

// program header，共 56 字节
struct elf64_phdr
{
        uint32_t p_type;   // 1 = PT_LOAD
        uint32_t p_flags;  // 4=R, 2=W, 1=X
        uint64_t p_offset; // 段内容在文件里的偏移
        uint64_t p_vaddr;  // 加载到虚拟地址
        uint64_t p_paddr;
        uint64_t p_filesz; // 文件里多大
        uint64_t p_memsz;  // 内存里多大（>= filesz）
        uint64_t p_align;
} __attribute__((packed));

#endif
