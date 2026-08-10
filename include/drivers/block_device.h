#ifndef _INC_BLOCK_DEVICE_
#define _INC_BLOCK_DEVICE_
#include <type.h>

// 块设备驱动接口
struct block_driver
{
        int (*read)(void *dev, uint64_t sector, void *buf);
        int (*write)(void *dev, uint64_t sector, const void *buf);
        uint64_t (*sector_count)(void *dev);
};

// 块设备驱动接口
struct block_device
{
        struct block_driver driver;
        void *private_data;
};

#endif