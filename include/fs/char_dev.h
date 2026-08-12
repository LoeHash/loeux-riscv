#ifndef _INC_CHAR_DEV
#define _INC_CHAR_DEV
#define MAX_CHAR_DEV 16
#include <type.h>

struct char_device_ops
{
        int (*open)(void *priv, int flags);
        int (*read)(void *priv, void *buf, uint64_t count, uint64_t *out_len);
        int (*write)(void *priv, const void *buf, uint64_t count, uint64_t *out_len);
        int (*close)(void *priv);
};

struct char_device
{
        const char *name;
        struct char_device_ops *ops;
        void *priv;
};

int vfs_register_chardev(const char *name, struct char_device_ops *ops, void *priv);
struct char_device *vfs_find_chardev(const char *name);

extern struct char_device char_devices[MAX_CHAR_DEV];
extern int char_dev_count;
#endif