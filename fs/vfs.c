#include <type.h>
#include <lib.h>
#include <vfs.h>
#include <spinlock.h>
#include <panic.h>
#include <fat12.h>
#include <char_dev.h>
#include <printk.h>

static struct mount_entry *vfs_find_mount(const char *path);
static int fd_check(int fd);
static int alloc_fd(struct file *file);
static void free_fd(int fd);

struct file *fd_table[MAX_FD_NUM];
struct mount_entry mount_points[MAX_MOUNT_NUM];
spinlock_t mount_lock;
spinlock_t fd_lock;
volatile uint32_t mount_idx = 0;

void init_vfs()
{
        init_spinlock(&mount_lock);
        init_spinlock(&fd_lock);
}

// 初始化标准输入输出
void init_vfs_std()
{
        // alloc_fd 从 0 开始分配
        vfs_open("/dev/ttyS0", FS_O_READ);  // fd = 0
        vfs_open("/dev/ttyS0", FS_O_WRITE); // fd = 1
        vfs_open("/dev/ttyS0", FS_O_WRITE); // fd = 2

        printk("stdin=0, stdout=1, stderr=2 -> /dev/ttyS0\n");
}

int vfs_create(const char *path, int is_dir)
{
        struct mount_entry *mp = vfs_find_mount(path);
        if (!mp)
                return -1;

        const char *rel_path = path + strlen(mp->mount_point);
        if (*rel_path == '/')
                rel_path++;
        if (*rel_path == '\0')
                return -1;

        return mp->fs_ops->fs_create(mp->fs_priv, rel_path, is_dir);
}

int vfs_close(int fd)
{
        if (fd_check(fd))
        {
                return -1;
        }

        struct file *file = fd_table[fd];
        if (!file)
        {
                return -1;
        }

        int ret;
        if (file->type == 1)
        {
                struct char_device *cdev = (struct char_device *)file->private;
                if (cdev->ops->close)
                {
                        ret = cdev->ops->close(cdev->priv);
                }
                else
                {
                        ret = 0;
                }
        }
        else if (file->type == 0)
        {
                ret = file->mnt->fs_ops->fs_close(file);
                if (ret < 0)
                {
                        return -1;
                }

                file->mnt->fs_ops->fs_free_node(file->private);
        }

        free_page(file);
        free_fd(fd);
        return 0;
}

int64_t vfs_write(int fd, const void *buf, uint64_t count)
{

        if (fd_check(fd) == -1)
        {
                return -1;
        }

        struct file *file = fd_table[fd];
        if (!file)
        {
                return -1;
        }

        if (!(file->flags & FS_O_WRITE) && !(file->flags & FS_O_RW))
        {
                return -1;
        }

        // 调用文件系统的写
        uint64_t out_len = 0;
        int ret;
        if (file->type == 1)
        {
                struct char_device *cdev = (struct char_device *)file->private;
                ret = cdev->ops->write(cdev->priv, buf, count, &out_len);
                if (ret < 0)
                {
                        return -1;
                }
                return out_len;
        }

        if (file->type == 0)
        {
                ret = file->mnt->fs_ops->fs_write(file, buf, count, &out_len);
                if (ret < 0)
                {
                        return -1;
                }

                file->pos += out_len;

                return out_len;
        }
}

int64_t vfs_read(int fd, void *buf, uint64_t count)
{
        if (fd_check(fd) == -1)
        {
                return -1;
        }

        struct file *file = fd_table[fd];
        if (!file)
        {
                return -1;
        }

        if (!(file->flags & FS_O_READ) && !(file->flags & FS_O_RW))
        {
                return -1;
        }

        // 根据不同type进行分流
        uint64_t out_len = 0;
        int ret;
        printk("file type: %d\n", file->type);
        if (file->type == 1)
        {
                struct char_device *cdev = (struct char_device *)file->private;
                ret = cdev->ops->read(cdev->priv, buf, count, &out_len);
                if (ret < 0)
                {
                        return -1;
                }
                return out_len;
        }

        if (file->type == 0)
        {
                // 调用文件系统的读
                ret = file->mnt->fs_ops->fs_read(file, buf, count, &out_len);
                if (ret < 0)
                {
                        return -1;
                }

                file->pos += out_len;
                return out_len;
        }
}

int vfs_open(const char *path, int flags)
{

        if (strncmp(path, DEV_PATH_PREFIX, DEV_PATH_PREFIX_LEN) == 0)
        {
                const char *dev_name = path + DEV_PATH_PREFIX_LEN;
                struct char_device *cdev = vfs_find_chardev(dev_name);
                if (!cdev)
                {
                        return -1;
                }

                struct file *file = alloc_page();
                if (!file)
                {
                        return -1;
                }

                file->mnt = NULL;
                file->private = cdev;
                file->flags = flags;
                file->pos = 0;
                file->type = 1; // 字符设备

                if (cdev->ops->open && cdev->ops->open(cdev->priv, flags) < 0)
                {
                        free_page(file);
                        return -1;
                }

                int fd = alloc_fd(file);
                if (fd < 0)
                {
                        free_page(file);
                        return -1;
                }
                return fd;
        }

        struct vfs_node *vnode = vfs_lookup(path);

        if (!vnode)
        {
                return -1;
        }

        struct file *file = alloc_page();
        if (!file)
        {
                vnode->mount->fs_ops->fs_free_node(vnode->private);
                free_page(vnode);
                return -1;
        }

        file->mnt = vnode->mount;
        file->private = vnode->private;
        file->flags = flags;
        file->pos = 0;
        file->type = 0;

        // 调用文件系统的open
        // 检查文件存在，权限等信息
        int ret = vnode->mount->fs_ops->fs_open(vnode->private, file, flags);
        if (ret == -1)
        {
                vnode->mount->fs_ops->fs_free_node(vnode->private);
                free_page(vnode);
                free_page(file);
                return -1;
        }

        int fd = alloc_fd(file);
        if (fd == -1)
        {
                vnode->mount->fs_ops->fs_close(file);
                vnode->mount->fs_ops->fs_free_node(vnode->private);
                free_page(vnode);
                free_page(file);
                return -1;
        }

        free_page(vnode);

        return fd;
}

/// @brief 返回 vfs_node
struct vfs_node *vfs_lookup(const char *path)
{
        struct mount_entry *mp = vfs_find_mount(path);

        if (mp == NULL)
        {
                return NULL;
        }

        const char *rel_path = path + strlen(mp->mount_point);
        if (*rel_path == '/')
        {
                rel_path++;
        }

        if (*rel_path == '\0')
        {
                rel_path = ".";
        }

        // priv 是一个文件系统的私有结构
        void *priv_node = NULL;
        int ret = mp->fs_ops->fs_lookup(mp->fs_priv, rel_path, &priv_node);
        if (ret < 0 || !priv_node)
        {
                return NULL;
        }

        struct vfs_node *vnode = alloc_page();

        if (!vnode)
        {
                // 文件系统负责释放 priv_node
                mp->fs_ops->fs_free_node(priv_node);
                return NULL;
        }

        vnode->mount = mp;
        vnode->private = priv_node;

        return vnode;
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

static struct mount_entry *vfs_find_mount(const char *path)
{
        struct mount_entry *the_best = NULL;
        int best_len = 0;

        for (int i = 0; i < mount_idx; i++)
        {
                struct mount_entry *mnt = &mount_points[i];
                int len = strlen(mnt->mount_point);

                // 如果路径以挂载点开头
                if (strncmp(path, mnt->mount_point, len) == 0)
                {

                        if (len > 1)
                        {
                                char next = path[len];
                                if (next != '/' && next != '\0')
                                {
                                        continue;
                                }
                        }

                        // 最长的
                        if (len > best_len)
                        {
                                best_len = len;
                                the_best = mnt;
                        }
                }
        }

        return the_best;
}

static void free_fd(int fd)
{
        if (fd_check(fd) == -1)
        {
                // 静默处理
                return;
        }

        acquire(&fd_lock);

        fd_table[fd] = NULL;

        release(&fd_lock);
}

static int alloc_fd(struct file *file)
{
        int fd = -1;
        acquire(&fd_lock);
        for (int i = 0; i < MAX_FD_NUM; i++)
        {
                if (fd_table[i] == NULL)
                {
                        fd = i;
                        fd_table[i] = file;
                        break;
                }
        }
        release(&fd_lock);
        return fd;
}

static int fd_check(int fd)
{

        if (fd < 0 || fd >= MAX_FD_NUM)
        {
                return -1;
        }

        return 0;
}

void print_mount_table(void)
{
        int i;
        int found = 0;

        printk("\n========== Mount Table ==========\n");
        printk("%-4s %-30s %-12s %-12s\n", "Idx", "Mount Point", "Device", "FS Type");
        printk("----------------------------------------\n");

        for (i = 0; i < MAX_MOUNT_NUM; i++)
        {
                struct mount_entry *entry = &mount_points[i];

                // 检查挂载点是否有效（假设空字符串表示未使用）
                if (entry->mount_point[0] == '\0')
                {
                        continue;
                }

                found = 1;

                // 打印索引和挂载点
                printk("%-4d %-30s ", i, entry->mount_point);

                // 打印设备地址
                if (entry->device != NULL)
                {
                        printk("%0#lx ", (unsigned long)entry->device);
                }
                else
                {
                        printk("%-12s ", "NULL");
                }

                // 打印文件系统类型（根据 fs_ops 地址判断，或者你可以添加 fs_type 字段）
                if (entry->fs_ops != NULL)
                {
                        printk("%0#lx", (unsigned long)entry->fs_ops);
                }
                else
                {
                        printk("%-12s", "NULL");
                }
                printk("\n");

                // 可选：打印更多详细信息
                printk("    fs_priv: ");
                if (entry->fs_priv != NULL)
                {
                        printk("%0#lx", (unsigned long)entry->fs_priv);
                }
                else
                {
                        printk("NULL");
                }
                printk("\n");

                // 如果有 block_device，打印其信息
                if (entry->device != NULL)
                {
                        printk("    sector_count: ");
                        uint64_t sectors = entry->device->driver.sector_count(entry->device->private_data);
                        printk("%ld\n", sectors);
                }
                printk("\n");
        }

        if (!found)
        {
                printk("No mount points found.\n");
        }

        printk("==================================\n");
}

void test_fat12_operations(void)
{
        char read_buf[512] = {0};
        const char *write_data = "Hello FAT12! This is a test write from kernel via VFS.";
        int fd, ret;
        int64_t bytes;
        struct file *file;

        printk("\n========== FAT12 File Operation Test ==========\n");

        printk("[Test 1] Listing root directory\n");
        struct fat12_priv *fs = (struct fat12_priv *)0x81ab2000;
        uint8_t *buf = alloc_page();
        if (buf)
        {
                for (uint32_t s = fs->root_dir_start; s < fs->root_dir_start + fs->root_dir_sectors; s++)
                {
                        fs->bdev->driver.read(fs->bdev->private_data, s, buf);
                        for (int i = 0; i < 16; i++)
                        {
                                struct fat12_dirent *e = (struct fat12_dirent *)(buf + i * 32);
                                if (e->dir_name[0] == 0x00)
                                {
                                        free_page(buf);
                                        goto test2;
                                }
                                if (e->dir_name[0] == 0xE5)
                                        continue;
                                if (e->dir_attr == 0x0F)
                                        continue;
                                if (e->dir_attr == 0x08)
                                        continue;
                                printk("  %.8s.%.3s size=%u cluster=%u\n",
                                       e->dir_name, e->dir_ext, e->dir_file_size, e->dir_first_cluster_low);
                        }
                }
                free_page(buf);
        }

test2:
        printk("\n[Test 2] Opening /hello.txt for read/write\n");
        fd = vfs_open("/hello.txt", FS_O_RW);
        if (fd < 0)
        {
                printk("  ✗ Failed to open /hello.txt (fd=%d)\n", fd);
                return;
        }
        printk("  ✓ File opened successfully, fd=%d\n", fd);

        printk("\n[Test 3] Reading from /hello.txt (first 16 bytes)\n");
        memset(read_buf, 0, sizeof(read_buf));
        bytes = vfs_read(fd, read_buf, 16);
        if (bytes < 0)
        {
                printk("  ✗ Read failed: %ld\n", bytes);
        }
        else
        {
                printk("  ✓ Read %ld bytes\n", bytes);
                printk("  Data: ");
                for (int i = 0; i < bytes && i < 16; i++)
                {
                        if (read_buf[i] >= 0x20 && read_buf[i] < 0x7F)
                                printk("%c", read_buf[i]);
                        else
                                printk(".");
                }
                printk("\n  Hex: ");
                for (int i = 0; i < bytes && i < 16; i++)
                {
                        printk("%0#lx ", (unsigned char)read_buf[i]);
                }
                printk("\n");
        }

        printk("\n[Test 4] Writing to /hello.txt (append)\n");
        file = fd_table[fd];
        if (file)
        {
                struct fat12_node *fnode = (struct fat12_node *)file->private;
                printk("  Current file size: %u bytes\n", fnode->file_size);
                file->pos = fnode->file_size;
        }
        bytes = vfs_write(fd, write_data, strlen(write_data));
        if (bytes < 0)
        {
                printk("  ✗ Write failed: %ld\n", bytes);
        }
        else
        {
                printk("  ✓ Write %ld bytes: \"%s\"\n", bytes, write_data);
        }

        printk("\n[Test 5] Reading back to verify write\n");
        if (file)
                file->pos = 0;
        memset(read_buf, 0, sizeof(read_buf));
        bytes = vfs_read(fd, read_buf, 512);
        if (bytes < 0)
        {
                printk("  ✗ Read back failed: %ld\n", bytes);
        }
        else
        {
                printk("  ✓ Read back %ld bytes\n", bytes);
                printk("  Full content:\n  ");
                for (int i = 0; i < bytes && i < 64; i++)
                {
                        if (read_buf[i] >= 0x20 && read_buf[i] < 0x7F)
                                printk("%c", read_buf[i]);
                        else
                                printk(".");
                }
                if (bytes > 64)
                        printk("...");
                printk("\n");
        }

        printk("\n[Test 6] Closing file\n");
        ret = vfs_close(fd);
        if (ret < 0)
        {
                printk("  ✗ Close failed: %d\n", ret);
        }
        else
        {
                printk("  ✓ File closed successfully\n");
        }

        printk("\n========== FAT12 File Operation Test Complete ==========\n");
}