#include <type.h>
#include <lib.h>
#include <vfs.h>
#include <spinlock.h>
#include <panic.h>
#include <fat12.h>

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

        int ret = file->mnt->fs_ops->fs_close(file);
        if (ret < 0)
        {
                return -1;
        }

        // 2. 释放文件系统节点
        file->mnt->fs_ops->fs_free_node(file->private);

        // 3. 释放 file 结构
        free_page(file);

        // 4. 清空 fd_table
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
        int ret = file->mnt->fs_ops->fs_write(file, buf, count, &out_len);
        if (ret < 0)
        {
                return -1;
        }

        file->pos += out_len;

        return out_len;
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

        // 4. 调用文件系统的读
        uint64_t out_len = 0;
        int ret = file->mnt->fs_ops->fs_read(file, buf, count, &out_len);
        if (ret < 0)
        {
                return -1;
        }

        file->pos += out_len;
        return out_len;
}

int vfs_open(const char *path, int flags)
{
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
        for (int i = 3; i < MAX_FD_NUM; i++)
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

        if (fd < 3 || fd >= MAX_FD_NUM)
        {
                return -1;
        }

        return 0;
}