#ifndef _INC_FDT
#define _INC_FDT
#include "../include/stdint.h"
#include "spinlock.h"

/* token */
// 所有token长度全部都是 32bit
#define FDT_BEGIN_NODE 0x00000001 // 节点开始
#define FDT_END_NODE 0x00000002   // 节点结束
#define FDT_PROP 0x00000003       // 属性定义
#define FDT_NOP 0x00000004        // 空操作（覆盖）
#define FDT_END 0x00000009        // 结构块结束

#define ROOT_PROP_COMPATIBLE "compatible"
#define ROOT_PROP_MODEL "model"
#define ROOT_PROP_ADDRESS_CELLS "#address-cells"
#define ROOT_PROP_SIZE_CELLS "#size-cells"
#define ROOT_PROP_SERIAL_NUMBER "serial-number"
#define ROOT_PROP_CHASSIS_TYPE "chassis-type"

#define ALIGN_UP(addr, align) (((addr) + (align) - 1) & ~((align) - 1))

#define GET_STR_OFFSET(offset) (ft_base_addr + fh_struct.off_dt_strings + offset)
#define GET_DT_OFFSET(offset) (uint64_t)(ft_base_addr + fh_struct.off_dt_struct + offset)

// FDT_BEGIN_NODE
//   空字符串（根节点名称为空，即一个 '\0'）
//   4字节对齐填充
//   属性1 (FDT_PROP)
//   属性2 (FDT_PROP)
//   ...
//   子节点1 (FDT_BEGIN_NODE ... FDT_END_NODE)
//   子节点2 (FDT_BEGIN_NODE ... FDT_END_NODE)
//   ...
// FDT_END_NODE
struct fdt_header
{
        uint32_t magic;             /* 固定值 0xd00dfeed */
        uint32_t totalsize;         /* 整个 DTB 文件的字节数（包括头部） */
        uint32_t off_dt_struct;     /* 结构块（struct block）相对于 DTB 起始的偏移 */
        uint32_t off_dt_strings;    /* 字符串块（strings block）相对于 DTB 起始的偏移 */
        uint32_t off_mem_rsvmap;    /* 内存预留块（memory reservation block）偏移 */
        uint32_t version;           /* 版本号，通常 >= 17 */
        uint32_t last_comp_version; /* 向下兼容的最低版本 */
        uint32_t boot_cpuid_phys;   /* 启动 CPU 的物理 ID */
        uint32_t size_dt_strings;   /* 字符串块的字节大小 */
        uint32_t size_dt_struct;    /* 结构块的字节大小 */
};

struct fdt_root_info
{
        char *compatible;        /* 指向字符串缓冲区： "str1\0str2\0\0" */
        uint32_t compatible_len; /* 总字节数（含所有\0） */
        char *model;             /* 指向字符串： "model_name\0" */
        uint32_t model_len;      /* 字节数 */
        uint32_t address_cells;  /* #address-cells 值 */
        uint32_t size_cells;     /* #size-cells 值 */
        char *serial_number;     /* "serial\0" */
        uint32_t serial_len;
        char *chassis_type;
        uint32_t chassis_len;
};

extern unsigned long ft_base_addr;
extern struct fdt_header fh_struct;
extern struct fdt_root_info fri_struct;
extern struct fdt_root_info fri_struct;
extern uint8_t *sub_node_base_addr;
extern uint8_t fdt_init_status;
extern spinlock_t fdt_init_lock;

void fdt_header_init();
uint32_t bte32(uint8_t *p);
uint64_t bte64(uint8_t *p);
void fdt_walk_nodes(unsigned long dtb,
                    int (*callback)(const char *name, int depth,
                                    void *node_ptr,
                                    void *data),
                    void *data);
void read_node_name(uint8_t *start, char *buf);
int read_node_prop(uint8_t *start, char *prop_name, void *dest, int _size);
void init_fdt(unsigned long ft_addr);
#endif