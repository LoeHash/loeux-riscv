#include <type.h>
#include <lib.h>
#include <vfs.h>
#include <spinlock.h>
#include <fat12.h>

struct file *fd_table[256];
struct mount_entry mount_points[MAX_MOUNT_NUM];
spinlock_t mount_lock;
volatile uint32_t mount_idx = 0;

void init_vfs()
{
        init_spinlock(&mount_lock);
}

int vfs_mount(char *mount_path, struct block_device *bdev, FSTYPE type)
{
        struct mount_entry *mnt;
        void *fs_priv = NULL;
        struct file_operation *ops = NULL;

        switch (type)
        {
        case FAT12:
                ops = &fat12_ops;
                fs_priv = ops->fs_mount(bdev);
                break;

        default:
                break;
        }

        if (fs_priv == NULL)
        {
                return -1;
        }

        acquire(&mount_lock);
        if (mount_idx >= MAX_MOUNT_NUM)
        {
                release(&mount_lock);
                return -1;
        }
        mnt = &mount_points[mount_idx++];
        release(&mount_lock);

        mnt->device = bdev;
        mnt->fs_ops = ops;
        mnt->fs_priv = fs_priv;
        int tmp_len = strlen(mount_path) + 1;
        memcpy(mnt->mount_point, mount_path, tmp_len > 32 ? 31 : tmp_len);
        if (tmp_len > 32)
        {
                mnt->mount_point[31] = '\0';
        }

        return 0;
}