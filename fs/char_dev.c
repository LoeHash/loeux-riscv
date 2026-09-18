#include <char_dev.h>
#include <panic.h>
#include <printk.h>
#include <lib.h>
#include <slab.h>
#include <hashmap.h>

static hash_table_t *char_dev_map = NULL;

/* 内部：取字符串键长度（含结尾 '\0'） */
static uint32_t cdev_key_len(const char *name)
{
    return (uint32_t)strlen(name) + 1;
}

int init_char_dev(void)
{
    if (char_dev_map != NULL)
        return 0;

    char_dev_map = hash_table_create(MAX_CHAR_DEV, NULL, NULL);
    if (char_dev_map == NULL)
    {
        panic_error("init_char_dev: hash_table_create failed\n");
        return -1;
    }

    return 0;
}

int vfs_register_chardev(const char *name, struct char_device_ops *ops, void *priv)
{
    if (!name || !ops)
        return -1;

    if (char_dev_map == NULL)
    {
        printk("vfs_register_chardev: char dev map not inited\n");
        return -1;
    }

    struct char_device *cdev = slab_alloc(sizeof(*cdev));
    if (cdev == NULL)
    {
        printk("vfs_register_chardev: slab_alloc failed\n");
        return -1;
    }

    cdev->name = name;
    cdev->ops  = ops;
    cdev->priv = priv;

    /*
     * key 直接存 name 指针（调用方保证生命周期，通常是静态字符串），
     * key_len 含结尾 '\0'，避免 "tty" 和 "ttyS0" 前缀混淆。
     * overwrite = false：同名设备重复注册视为失败。
     */
    if (!hash_table_insert_if_absent(char_dev_map,
                                     name, cdev_key_len(name),
                                     cdev))
    {
        slab_free(cdev);
        printk("vfs_register_chardev: '%s' already registered\n", name);
        return -1;
    }

    printk("Char device registered: %s\n", name);
    return 0;
}

struct char_device *vfs_find_chardev(const char *name)
{
    if (name == NULL || char_dev_map == NULL)
        return NULL;

    return (struct char_device *)hash_table_lookup(char_dev_map,
                                                   name,
                                                   cdev_key_len(name));
}