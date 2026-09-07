#include <type.h>
#include <lib.h>
#include <vfs.h>
#include <spinlock.h>
#include <panic.h>
#include <fat12.h>
#include <char_dev.h>
#include <printk.h>
#include <proc.h>

static struct mount_entry *vfs_find_mount(const char *path);
static int fd_check(int fd);
static int alloc_fd(struct file *file);

// 注意：fd 现在是 per-process 的，存放在 get_task()->ofile[NOFILE] 中。
// 全局 fd_table 已移除。alloc_fd/free_fd/fd_check 操作当前 task 的 ofile[]，
// 用 ts->lk 保护，避免与 fork 复制、scheduler 等路径的竞态。
struct mount_entry mount_points[MAX_MOUNT_NUM];
spinlock_t mount_lock;
spinlock_t fd_lock;
volatile uint32_t mount_idx = 0;

void init_vfs()
{
        init_spinlock(&mount_lock);
        init_spinlock(&fd_lock);
}

// 初始化标准输入输出：在当前 task 的 ofile[] 中分配 fd 0/1/2。
// 必须在 task 上下文调用（依赖 get_task()），典型调用点为 first_ret()。
// boot 阶段没有 current task，故不能在 kstart 里直接调用。
void init_vfs_std()
{
        // alloc_fd 从 0 开始分配
        vfs_open("/dev/ttyS0", FS_O_READ);  // fd = 0
        vfs_open("/dev/ttyS0", FS_O_WRITE); // fd = 1
        vfs_open("/dev/ttyS0", FS_O_WRITE); // fd = 2

        printk("stdin=0, stdout=1, stderr=2 -> /dev/ttyS0\n");
}

// 设置文件偏移
int vfs_seek(int fd, uint64_t offset)
{
        if (fd_check(fd) == -1)
        {
                return -1;
        }

        struct file *f = get_task()->ofile[fd];

        if (!f || offset > f->size)
                return -1;

        f->pos = offset;
        return 0;
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

        struct task_struct *ts = get_task();
        struct file *file;
        acquire(&ts->lk);
        file = ts->ofile[fd];
        if (!file)
        {
                release(&ts->lk);
                return -1;
        }
        // 先摘除 ofile 槽位，再释放引用，
        // 避免 file_close 在持锁时阻塞过久，也避免重复 close。
        ts->ofile[fd] = NULL;
        release(&ts->lk);

        file_close(file);
        return 0;
}

// 释放对 struct file 的一次引用。
// refcount 归 0 时才真正调用底层 fs_close / cdev close 并 free_page。
// 多个进程通过 fork 共享同一 struct file 时，各自 close 只 decref，
// 最后一个 close 才回收底层资源（Unix 经典语义）。
void file_close(struct file *file)
{
        if (!file)
        {
                return;
        }
        if (__atomic_sub_fetch(&file->refcount, 1, __ATOMIC_ACQ_REL) > 0)
        {
                return;
        }

        // 最后一个引用：回收底层资源
        if (file->type == 1)
        {
                struct char_device *cdev = (struct char_device *)file->private;
                if (cdev->ops->close)
                {
                        cdev->ops->close(cdev->priv);
                }
        }
        else if (file->type == 0)
        {
                if (file->mnt->fs_ops->fs_close(file) < 0)
                {
                        // 底层关闭失败仍要释放 file 结构，避免泄漏
                }
                file->mnt->fs_ops->fs_free_node(file->private);
        }

        free_page(file);
}

int64_t vfs_write(int fd, const void *buf, uint64_t count)
{

        if (fd_check(fd) == -1)
        {
                return -1;
        }

        struct file *file = get_task()->ofile[fd];
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
        return -1;
}

int64_t vfs_read(int fd, void *buf, uint64_t count)
{
        if (fd_check(fd) == -1)
        {
                return -1;
        }

        struct file *file = get_task()->ofile[fd];
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
        return -1;
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
                file->type = 1;     // 字符设备
                file->refcount = 1; // 首次打开，引用计数为 1

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
        file->size = vnode->size;
        file->refcount = 1; // 首次打开，引用计数为 1

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
        vnode->size = ret;

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

// 在当前 task 的 ofile[] 中找一个空槽位存放 file。
// 用 ts->lk 保护，与 fork 复制 ofile、scheduler 等路径互斥。
static int alloc_fd(struct file *file)
{
        struct task_struct *ts = get_task();
        int fd = -1;
        acquire(&ts->lk);
        for (int i = 0; i < NOFILE; i++)
        {
                if (ts->ofile[i] == NULL)
                {
                        fd = i;
                        ts->ofile[i] = file;
                        break;
                }
        }
        release(&ts->lk);
        return fd;
}

static int fd_check(int fd)
{
        // fd 是 per-process 的，范围 [0, NOFILE)
        if (fd < 0 || fd >= NOFILE)
        {
                return -1;
        }

        return 0;
}
