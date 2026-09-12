#include <slab.h>
#include <panic.h>
/*
Cache
 │
 │ 管理很多
 ▼
Slab
 │
 │ 包含很多
 ▼
Object

*/
/// @brief slab 对齐大小
/// @param size 大小
/// @return 对齐后的大小
static inline uint32_t slab_round_up(uint32_t size)
{
        if (size == 0 || size > SLAB_SIZE(4096))
                return 0;

        if (size <= SLAB_SIZE(8))
                return SLAB_SIZE(8);

        size--;
        size |= size >> 1;
        size |= size >> 2;
        size |= size >> 4;
        size |= size >> 8;
        size |= size >> 16;
        size++;

        return size;
}