#include <fdt.h>
#include <printk.h>
#include <memory.h>
#include <lib.h>
#include <type.h>
#include <spinlock.h>

static void print_fdt_header(struct fdt_header *hdr);
static uint32_t CHAR_BUFF_SIZE = 128;

struct fdt_header fh_struct = {0};
struct fdt_root_info fri_struct = {0};
uint8_t *sub_node_base_addr;

// init things....
uint8_t fdt_init_status = 0;
spinlock_t fdt_init_lock = {0};

// this is callback!
int walk_fdt(const char *name, int depth,
             void *node_ptr,
             void *data)
{
        // 打印缩进
        for (int i = 0; i < depth; i++)
                printk("  ");

        // 打印节点名称和地址
        printk("%s (0x%p)\n", name, node_ptr);

        return 0;
}

void init_fdt(unsigned long ft_addr)
{
        acquire(&fdt_init_lock);
        if (fdt_init_status == 1)
        {
                release(&fdt_init_lock);
                return;
        }

        ft_base_addr = ft_addr;
        // printk("ft_base_addr = 0x%lx\n", ft_base_addr);
        fdt_header_init();
        printk("fdt header and root node has been inited :) \n");
        fdt_walk_nodes((uint64_t)sub_node_base_addr, walk_fdt, 0);
        printk("Successfully Detected the fdt infomation! :)\n");
        printk("the fdt size = %ld\n", fh_struct.totalsize);
        printk("Successfully inited the fdt! :)\n");

        fdt_init_status = 1;
        release(&fdt_init_lock);
}

// 当前指针指向 FDT_PROP
// 返回后指向下一个 FDT_PROP/ FDT_END_NODE / FDT_BEGIN_NODE
void skip_prop(uint8_t **start_ptr)
{
        uint8_t *p = *start_ptr;
        p += 4;
        // 获取长度
        int len = bte32(p);
        p += 8;
        p += len;
        p = (uint8_t *)ALIGN_UP((uintptr_t)p, 4);
        *start_ptr = p;
}

/// @brief 读取对应数据，写回到dest 大端
///        写回的数据必须由调用者维护
/// @param start        node 节点开始
/// @param prop_name    属性名称
/// @param dest         写回地址指针
/// @param _size        写回地址的缓冲区大小
/// @return             len: success, 0: not, -1: get wrong
int read_node_prop(uint8_t *start, char *prop_name, void *dest, int _size)
{
        if ((uint32_t)bte32(start) != FDT_BEGIN_NODE)
        {
                printk("read_node_prop: the start ptr is not a node!\n");
                return -1;
        }

        int len = strlen(start + 4);
        if (len > CHAR_BUFF_SIZE - 1)
        {
                printk("read_node_prop: excepted the length of node prop name less than %u!\n", CHAR_BUFF_SIZE);
                return -1;
        }
        start += 4;
        start += len + 1;
        start = (uint8_t *)ALIGN_UP((uintptr_t)start, 4);

        while (bte32(start) == FDT_PROP)
        {
                start += 4;
                len = (uint32_t)bte32(start);
                start += 4;
                int name_off = (uint32_t)bte32(start);
                start += 4;
                if (strcmp((char *)GET_STR_OFFSET(name_off), prop_name) != 0)
                {
                        // move to next prop.
                        start += len;
                        start = (uint8_t *)ALIGN_UP((uintptr_t)start, 4);
                        while ((uint32_t)bte32(start) == FDT_NOP)
                        {
                                start += 4;
                        }
                        continue;
                }

                if (len > _size)
                {
                        printk("too short for dest size!\n");
                        return -1;
                }

                memcpy(dest, start, len);

                return len;
        }
        return 0;
}

// 需要start 为node起始
void read_node_name(uint8_t *start, char *buf)
{
        int len = strlen(start + 4);
        if (len > CHAR_BUFF_SIZE - 1)
        {
                printk("read_node_name: excepted the length of node less than %u!\n", CHAR_BUFF_SIZE);
                return;
        }

        memcpy(buf, (start + 4), len);
        buf[len] = '\0';
}

// 跳过当前节点（包括所有子节点），返回时 ptr 指向该节点对应的 FDT_END_NODE 之后
void skip_subtree(uint8_t **ptr)
{
        uint8_t *p = *ptr;
        // 跳过 FDT_BEGIN_NODE
        p += 4;
        // 跳过节点名（含 \0 并对齐）
        p += strlen((const char *)p) + 1;
        p = (uint8_t *)ALIGN_UP((uintptr_t)p, 4);
        // 循环处理节点内部所有 token
        while (1)
        {
                uint32_t token = bte32(p);
                if (token == FDT_END_NODE)
                {
                        p += 4;
                        break;
                }
                else if (token == FDT_PROP)
                {
                        // 跳过属性
                        p += 4;
                        uint32_t len = bte32(p);
                        p += 8 + len;
                        p = (uint8_t *)ALIGN_UP((uintptr_t)p, 4);
                }
                else if (token == FDT_BEGIN_NODE)
                {
                        // 递归跳过子节点
                        *ptr = p;
                        skip_subtree(ptr);
                        p = *ptr;
                }
                else if (token == FDT_NOP)
                {
                        p += 4;
                }
                else
                {
                        break; // 未知 token，出错退出
                }
        }
        *ptr = p;
}
// 递归处理一个节点
// 返回 0 继续，-1 停止整个遍历
int walk_node(uint8_t **ptr, int depth,
              int (*callback)(const char *name, int depth, void *node_ptr, void *data),
              void *data)
{
        uint8_t *p = *ptr;
        uint32_t token = bte32(p);
        if (token != FDT_BEGIN_NODE)
        {
                printk("walk_node: expected FDT_BEGIN_NODE, got 0x%x\n", token);
                return -1;
        }
        // 读取节点名
        const char *name = (const char *)(p + 4);
        int ret = callback(name, depth, p, data);
        if (ret == -1)
        {
                // 停止遍历，但也要跳过这个节点以免破坏后续解析
                *ptr = p;
                skip_subtree(ptr);
                return -1;
        }
        if (ret == 1)
        {
                // 跳过整个子树
                *ptr = p;
                skip_subtree(ptr);
                return 0;
        }
        // ret == 0：进入节点内部
        p += 4;                // 跳过 FDT_BEGIN_NODE
        p += strlen(name) + 1; // 跳过节点名
        p = (uint8_t *)ALIGN_UP((uintptr_t)p, 4);
        // 遍历节点内部所有 token
        while (1)
        {
                token = bte32(p);
                if (token == FDT_END_NODE)
                {
                        p += 4;
                        *ptr = p;
                        return 0;
                }
                else if (token == FDT_PROP)
                {
                        skip_prop(&p);
                }
                else if (token == FDT_BEGIN_NODE)
                {
                        *ptr = p;
                        if (walk_node(ptr, depth + 1, callback, data) == -1)
                        {
                                // 子进程要求停止，直接向上返回 -1
                                return -1;
                        }
                        p = *ptr;
                }
                else if (token == FDT_NOP)
                {
                        p += 4;
                }
                else
                {
                        printk("walk_node: unknown token 0x%x at depth %d\n", token, depth);
                        return -1;
                }
        }
}
// 外部入口，从根节点开始遍历
void fdt_walk_nodes(unsigned long dtb,
                    int (*callback)(const char *name, int depth,
                                    void *node_ptr, void *data),
                    void *data)
{
        uint8_t *ptr = (uint8_t *)dtb;

        while (bte32(ptr) == FDT_BEGIN_NODE)
        {
                uint32_t token = bte32(ptr);
                if (token == FDT_BEGIN_NODE)
                {
                        int res = walk_node(&ptr, 0, callback, data);
                        if (res == -1)
                                return;
                }
                else if (token == FDT_END)
                {
                        return; // 正常结束
                }
                else if (token == FDT_NOP)
                {
                        ptr += 4; // 跳过填充
                }
                else
                {
                        printk("fdt_walk_nodes: unexpected token 0x%x\n", token);
                        return;
                }
        }
}

/*
 * ============================================================================
 *                        DTB (Device Tree Blob) 内存布局
 * ============================================================================
 *
 *  基地址 (ft_base_addr)
 *  +=============================+ <-- 0x0000
 *  |     fdt_header              |  大小为 40 字节 (0x28)
 *  |     (固定结构)              |
 *  +-----------------------------+ <-- fh_struct.off_mem_rsvmap (通常 0x28)
 *  |     Memory Reservation      |  内存预留块
 *  |     Block                  |
 *  |     (8字节对齐)            |
 *  |     - entry 1: addr, size  |
 *  |     - entry 2: addr, size  |
 *  |     - ...                  |
 *  |     - entry N: 0, 0 (结束) |
 *  +-----------------------------+ <-- fh_struct.off_dt_struct (通常 0x38)
 *  |     Structure Block         |  结构块 (4字节对齐)
 *  |                            |
 *  |  +-------------------------+
 *  |  | FDT_BEGIN_NODE (0x1)   |  4字节, 根节点开始
 *  |  +-------------------------+
 *  |  | 节点名称                |  根节点名称为空: '\0' (1字节)
 *  |  | (以'\0'结尾)           |
 *  |  +-------------------------+
 *  |  | 对齐填充                |  补齐到 4 字节边界
 *  |  +-------------------------+
 *  |  |                         |
 *  |  |  属性1 (FDT_PROP)       |
 *  |  |  +---------------------+
 *  |  |  | FDT_PROP (0x3)     |  4字节, 属性令牌
 *  |  |  +---------------------+
 *  |  |  | len                |  4字节, 属性值长度
 *  |  |  +---------------------+
 *  |  |  | nameoff            |  4字节, 属性名在字符串块中的偏移
 *  |  |  +---------------------+
 *  |  |  | value              |  len 字节, 属性值
 *  |  |  +---------------------+
 *  |  |  | 对齐填充            |  补齐到 4 字节边界
 *  |  |  +---------------------+
 *  |  |                         |
 *  |  |  属性2 (FDT_PROP)       |
 *  |  |  ...                   |
 *  |  |                         |
 *  |  |  子节点1                |
 *  |  |  +---------------------+
 *  |  |  | FDT_BEGIN_NODE     |  4字节, 子节点开始
 *  |  |  +---------------------+
 *  |  |  | 节点名称            |  "memory\0"
 *  |  |  +---------------------+
 *  |  |  | 对齐填充            |
 *  |  |  +---------------------+
 *  |  |  | 属性...            |
 *  |  |  +---------------------+
 *  |  |  | FDT_END_NODE (0x2) |  4字节, 子节点结束
 *  |  |  +---------------------+
 *  |  |                         |
 *  |  |  子节点2                |
 *  |  |  ...                   |
 *  |  |                         |
 *  |  +-------------------------+
 *  |  | FDT_END_NODE (0x2)     |  4字节, 根节点结束
 *  |  +-------------------------+
 *  |  | FDT_END (0x9)          |  4字节, 结构块结束
 *  |  +-------------------------+
 *  |                            |
 *  +-----------------------------+ <-- fh_struct.off_dt_strings
 *  |     Strings Block           |  字符串块
 *  |                            |
 *  |  +-------------------------+
 *  |  | "compatible\0"          |  属性名字符串
 *  |  +-------------------------+
 *  |  | "model\0"              |
 *  |  +-------------------------+
 *  |  | "#address-cells\0"     |
 *  |  +-------------------------+
 *  |  | "#size-cells\0"        |
 *  |  +-------------------------+
 *  |  | ...                    |
 *  |  +-------------------------+
 *  |                            |
 *  +=============================+ <-- ft_base_addr + fh_struct.totalsize
 *
 * ============================================================================
 *                                令牌说明
 * ============================================================================
 *  FDT_BEGIN_NODE  0x00000001   节点开始
 *  FDT_END_NODE    0x00000002   节点结束
 *  FDT_PROP        0x00000003   属性定义
 *  FDT_NOP         0x00000004   空操作 (占位符)
 *  FDT_END         0x00000009   结构块结束
 *
 * ============================================================================
 *                              属性结构 (FDT_PROP)
 * ============================================================================
 *  +----------+----------+----------+----------+
 *  | FDT_PROP |   len    | nameoff  |  value   |
 *  | (0x3)    | (4字节)  | (4字节)  | (len字节) |
 *  +----------+----------+----------+----------+
 *
 *  nameoff: 属性名在字符串块中的偏移量
 *  len:     属性值的字节长度
 *  value:   属性值数据
 *
 * ============================================================================
 *                              节点结构
 * ============================================================================
 *  节点 = FDT_BEGIN_NODE + 名称 + 对齐 + 属性列表 + 子节点列表 + FDT_END_NODE
 *
 *  根节点特殊之处:
 *    1. 名称为空字符串 (只有一个 '\0')
 *    2. 根节点结束后紧跟 FDT_END 标记结构块结束
 *
 * ============================================================================
 *                              解析流程
 * ============================================================================
 *  1. 读取 fdt_header (40 字节)
 *  2. 通过 off_dt_struct 定位到结构块
 *  3. 跳过 FDT_BEGIN_NODE (4字节) + 空名称 (1字节) + 对齐填充
 *  4. 循环读取属性:
 *     a. 检查令牌是否为 FDT_PROP
 *     b. 读取 len 和 nameoff
 *     c. 通过 nameoff + off_dt_strings 获取属性名
 *     d. 根据属性名处理 value
 *     e. 跳转到下一个属性: ALIGN_UP(当前位置 + len, 4)
 *  5. 遇到 FDT_BEGIN_NODE (子节点) 或 FDT_END_NODE 停止
 *  6. 子节点的解析交给其他函数
 *
 * ============================================================================
 *                              注意事项
 * ============================================================================
 *  1. 所有多字节整数都是大端序，需要 bte32()/bte64() 转换
 *  2. 字符串不需要字节序转换
 *  3. 结构块 4 字节对齐，字符串块没有对齐要求
 *  4. 内存预留块 8 字节对齐
 * ============================================================================
 */
void fdt_header_init()
{
        uint8_t *ch = (uint8_t *)ft_base_addr;
        uint32_t *fh_ptr = (uint32_t *)&fh_struct;
        uint32_t size = 0;
        while (size < sizeof(struct fdt_header))
        {
                *(fh_ptr + size / 4) = bte32(ch + size);
                size += 4;
        }
        print_fdt_header(&fh_struct);

        // 解析根节点
        uint8_t *root_ptr = (uint8_t *)(ft_base_addr + fh_struct.off_dt_struct);
        printk("the root node start at: %08x\n", root_ptr);
        printk("the root node start flag: %u\n", bte32(root_ptr));
        root_ptr = (uint8_t *)ALIGN_UP((uintptr_t)(root_ptr + 5), 4);
        int prop_len;
        char *prop_name;

        while (bte32(root_ptr) == FDT_PROP)
        {

                root_ptr += 4;
                printk("found a prop!\n");
                prop_len = bte32(root_ptr);
                printk("\t\tprop len: %u\n", prop_len);
                root_ptr += 4;
                prop_name = (char *)(uintptr_t)GET_STR_OFFSET(bte32(root_ptr));
                printk("\t\tprop name: %s\n", prop_name);
                root_ptr += 4;
                if (strcmp(ROOT_PROP_COMPATIBLE, prop_name) == 0)
                {
                        fri_struct.compatible_len = prop_len;
                        fri_struct.compatible = (char *)root_ptr;
                        printk("\t\tthe compatible: %s\n", fri_struct.compatible);
                }
                else if (strcmp(ROOT_PROP_MODEL, prop_name) == 0)
                {
                        fri_struct.model_len = prop_len;
                        fri_struct.model = (char *)root_ptr;
                        printk("\t\tthe model: %s\n", fri_struct.model);
                }
                else if (strcmp(ROOT_PROP_ADDRESS_CELLS, prop_name) == 0)
                {
                        fri_struct.address_cells = bte32(root_ptr);
                        printk("\t\tthe address_cells is: %u\n", fri_struct.address_cells);
                }
                else if (strcmp(ROOT_PROP_SIZE_CELLS, prop_name) == 0)
                {
                        fri_struct.size_cells = bte32(root_ptr);
                        printk("\t\tthe size_cells is: %u\n", fri_struct.size_cells);
                }

                root_ptr = (uint8_t *)ALIGN_UP((uintptr_t)(root_ptr + prop_len), 4);
                printk("=================================\n");
        }

        // 第一个子节点的起始位置
        if (bte32(root_ptr) == FDT_BEGIN_NODE)
        {
                sub_node_base_addr = root_ptr;
        }
}

uint32_t bte32(uint8_t *p)
{
        return (uint32_t)((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3]);
}

uint64_t bte64(uint8_t *p)
{
        return (uint64_t)((uint64_t)bte32(p) << 32 | bte32(p + 4));
}

static void print_fdt_header(struct fdt_header *hdr)
{
        printk("========== FDT Header ==========\n");
        printk("magic:             0x%08x\n", hdr->magic);
        printk("totalsize:         0x%08x (%u bytes)\n", hdr->totalsize, hdr->totalsize);
        printk("off_dt_struct:     0x%08x (%u)\n", hdr->off_dt_struct, hdr->off_dt_struct);
        printk("off_dt_strings:    0x%08x (%u)\n", hdr->off_dt_strings, hdr->off_dt_strings);
        printk("off_mem_rsvmap:    0x%08x (%u)\n", hdr->off_mem_rsvmap, hdr->off_mem_rsvmap);
        printk("version:           0x%08x (%u)\n", hdr->version, hdr->version);
        printk("last_comp_version: 0x%08x (%u)\n", hdr->last_comp_version, hdr->last_comp_version);
        printk("boot_cpuid_phys:   0x%08x (%u)\n", hdr->boot_cpuid_phys, hdr->boot_cpuid_phys);
        printk("size_dt_strings:   0x%08x (%u bytes)\n", hdr->size_dt_strings, hdr->size_dt_strings);
        printk("size_dt_struct:    0x%08x (%u bytes)\n", hdr->size_dt_struct, hdr->size_dt_struct);
        printk("================================\n");
}