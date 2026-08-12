#include <char_dev.h>
#include <printk.h>
#include <lib.h>

struct char_device char_devices[MAX_CHAR_DEV] = {0};
int char_dev_count = 0;

int vfs_register_chardev(const char *name, struct char_device_ops *ops, void *priv)
{
        if (!name || !ops)
        {
                return -1;
        }

        if (char_dev_count >= MAX_CHAR_DEV)
        {
                printk("Char device table full\n");
                return -1;
        }

        char_devices[char_dev_count].name = name;
        char_devices[char_dev_count].ops = ops;
        char_devices[char_dev_count].priv = priv;
        char_dev_count++;

        printk("Char device registered: %s\n", name);
        return 0;
}

struct char_device *vfs_find_chardev(const char *name)
{
        for (int i = 0; i < char_dev_count; i++)
        {
                if (strcmp(char_devices[i].name, name) == 0)
                {
                        return &char_devices[i];
                }
        }
        return NULL;
}