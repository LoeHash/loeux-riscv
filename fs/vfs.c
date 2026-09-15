#include <vfs.h>
#include <panic.h>
#include <sleeplock.h>
#include <slab.h>
#include <spinlock.h>
#include <proc.h>
#include <memory.h>
#include <fat12.h>
#include <block_device.h>
#include <lib.h>

static spinlock_t inum_lk = {0};
static volatile uint64_t _vfs_inum_counter = 0;
static mount_table_t mount_table = {0};
static filesystem_registry_t fs_registry = {0};
static uint64_t vfs_alloc_inum();
static void vfs_filesystem_init(void);

void init_vfs()
{
        init_spinlock(&inum_lk);
        inin_spinlock(&mount_table.lock);
        vfs_filesystem_init();
}

int vfs_mount(struct block_device *dev,
              const char *target,
              const char *fs_type)
{
        struct filesystem *fs = NULL;
        struct super_block *sb = NULL;
        struct mount *mnt = NULL;
        struct mount *parent = NULL;

        if (dev == NULL || target == NULL || fs_type == NULL)
        {
                return -1;
        }

        struct mount *existing = vfs_find_mount(target);

        if (existing != NULL &&
            strcmp(existing->path, target) == 0)
        {
                goto went_error;
        }

        // 得到fs实例
        fs = vfs_get_filesystem(fs_type);

        if (fs == NULL)
        {
                goto went_error;
        }

        // 从fs中构建好superblock
        if (fs->get_super(fs, dev, &sb) < 0)
        {
                goto went_error;
        }

        if (sb == NULL)
        {
                goto went_error;
        }

        mnt = slab_alloc(sizeof(*mnt));
        if (mnt == NULL)
        {
                goto went_error;
        }

        memset(mnt, 0, sizeof(*mnt));

        // 设置挂载点
        strcpy(mnt->path, target);

        // 设置此挂载点的sb
        mnt->sb = sb;

        // 分流root
        if (strcmp(target, "/") == 0)
        {
                acquire(&mount_table.lock);

                if (mount_table.root != NULL)
                {
                        release(&mount_table.lock);
                        goto went_error;
                }

                mnt->parent = NULL;
                mount_table.root = mnt;

                release(&mount_table.lock);

                return 0;
        }

        parent = vfs_find_mount(target);
        if (parent == NULL)
        {
                goto went_error;
        }

        mnt->parent = parent;

        // 加入 mount tree
        acquire(&mount_table.lock);

        mnt->next = parent->child;
        parent->child = mnt;

        release(&mount_table.lock);

        return 0;

went_error:
        if (mnt != NULL)
        {
                slab_free(mnt);
        }

        if (sb != NULL && fs != NULL)
        {
                fs->kill_sb(sb);
        }

        return -1;
}

/// @brief 在 path 指定的位置创建普通文件，并返回新文件 inode。
/// @param path
/// @param mode         创建模式
/// @param inode        inode 指针
/// @return 成功 0
int vfs_create(const char *path,
               uint32_t mode,
               struct inode **inode)
{
        if (path == NULL || inode == NULL)
                return -1;

        struct inode *parent = NULL;

        char *name = slab_alloc(VFS_MAX_PATH_LEN);

        if (name == NULL)
                return -1;

        if (vfs_lookup_parent(path, &parent, name) < 0)
        {
                slab_free(name);
                return -1;
        }

        if (parent == NULL ||
            parent->iops == NULL ||
            parent->iops->create == NULL)
        {
                slab_free(name);
                return -1;
        }

        if ((parent->mode & S_IFMT) != S_IFDIR)
        {
                slab_free(name);
                return -1;
        }

        /*
         * 创建目录项要求父目录具有：
         *
         * MAY_WRITE : 修改目录内容
         * MAY_EXEC  : 搜索/访问目录
         */
        if (vfs_permission(parent,
                           &get_task()->cred,
                           MAY_WRITE | MAY_EXEC) < 0)
        {
                slab_free(name);
                return -1;
        }

        /*
         * VFS 对外统一只接受权限位。
         * 普通文件类型由 VFS 负责补上。
         */
        mode &= 0777;

        int ret = parent->iops->create(parent,
                                       name,
                                       S_IFREG | mode,
                                       inode);

        slab_free(name);

        return ret;
}

int vfs_lookup(const char *path, struct inode **inode)
{
        if (path == NULL || inode == NULL || path[0] != '/')
        {

                return -1;
        }

        struct mount *mnt = vfs_find_mount(path);

        if (mnt == NULL || mnt->sb == NULL || mnt->sb->root == NULL)
        {
                return -1;
        }

        struct inode *current = mnt->sb->root;

        // 计算相对于 mount root 的路径
        const char *relative = path + strlen(mnt->path);

        if (*relative == '\0')
        {
                *inode = current;
                return 0;
        }

        if (*relative != '/')
                return -1;

        relative++;

        char *name = slab_alloc(VFS_MAX_PATH_LEN);
        if (name == NULL)
        {
                return -1;
        }

        uint64_t len;
        struct task_struct *task = get_task();

        while (*relative != '\0')
        {
                const char *slash = strchr(relative, '/');

                if (slash != NULL)
                        len = slash - relative;
                else
                        len = strlen(relative);

                if (len == 0)
                {
                        relative++;
                        continue;
                }

                if (len >= VFS_MAX_PATH_LEN)
                {
                        slab_free(name);
                        return -1;
                }

                memcpy(name, relative, len);
                name[len] = '\0';

                if (vfs_permission(current,
                                   &task->cred,
                                   MAY_EXEC) < 0)
                {
                        slab_free(name);
                        return -1;
                }
                if (current->iops == NULL ||
                    current->iops->lookup == NULL)
                {
                        slab_free(name);
                        return -1;
                }

                struct inode *next = NULL;

                if (current->iops->lookup(current,
                                          name,
                                          &next) < 0)
                {
                        slab_free(name);
                        return -1;
                }

                if (next == NULL)
                {
                        slab_free(name);
                        return -1;
                }

                current = next;

                relative += len;

                if (*relative == '/')
                        relative++;
        }

        slab_free(name);
        *inode = current;

        return 0;
}

struct filesystem *vfs_get_filesystem(const char *name)
{
        if (name == NULL)
                return NULL;

        acquire(&fs_registry.lock);

        struct filesystem *fs = fs_registry.head;

        while (fs != NULL)
        {
                if (strcmp(fs->name, name) == 0)
                        break;

                fs = fs->next;
        }

        release(&fs_registry.lock);

        return fs;
}

/// @brief 给定一个绝对路径 path，找到 覆盖该路径的最深层 mount。
struct mount *vfs_find_mount(const char *path)
{
        if (path == NULL || path[0] != '/')
                return NULL;

        struct mount *mnt = mount_table.root;

        if (mnt == NULL)
                return NULL;

        while (1)
        {
                struct mount *child = mnt->child;

                while (child != NULL)
                {
                        if (mount_path_match(child->path, path))
                                break;

                        child = child->next;
                }

                if (child == NULL)
                        return mnt;

                mnt = child;
        }
}

/// @brief  解析 path，找到其直接父目录 inode
///         并将最后一个路径组件写入 name。父目录不存在或路径非法则失败。
/// @return 失败返回非0值
int vfs_lookup_parent(const char *path,
                      struct inode **parent,
                      char *name)
{
        if (path == NULL || parent == NULL || name == NULL)
                return -1;

        if (path[0] != '/')
                return -1;

        size_t path_len = strlen(path);

        if (path_len <= 1)
                return -1;

        /*
         * 找最后一个 '/'
         */
        const char *slash = strrchr(path, '/');

        if (slash == NULL)
                return -1;

        // 最后一个 component

        const char *last = slash + 1;

        if (*last == '\0')
                return -1;

        size_t name_len = strlen(last);

        if (name_len >= VFS_MAX_PATH_LEN)
                return -1;

        memcpy(name, last, name_len);
        name[name_len] = '\0';

        // 构造父路径
        char *parent_path = slab_alloc(VFS_MAX_PATH_LEN);

        if (parent_path == NULL)
                return -1;

        size_t parent_len = slash - path;

        if (parent_len >= VFS_MAX_PATH_LEN)
        {
                slab_free(parent_path);
                return -1;
        }

        memcpy(parent_path, path, parent_len);
        parent_path[parent_len] = '\0';

        // 父路径为根目录

        if (parent_len == 0)
        {
                parent_path[0] = '/';
                parent_path[1] = '\0';
        }

        int ret = vfs_lookup(parent_path, parent);

        slab_free(parent_path);

        return ret;
}

// fs对象的生命周期在运行期间是不会被销毁的
/// @brief 注册fs到fs注册表中
/// 0成功，其他错误
int vfs_register_filesystem(struct filesystem *fs)
{
        if (fs == NULL)
                return -1;

        acquire(&fs_registry.lock);

        // 检查同名 filesystem
        struct filesystem *p = fs_registry.head;

        while (p != NULL)
        {
                if (strcmp(p->name, fs->name) == 0)
                {
                        release(&fs_registry.lock);
                        return -1;
                }

                p = p->next;
        }

        fs->next = fs_registry.head;
        fs_registry.head = fs;

        release(&fs_registry.lock);

        return 0;
}

/// @brief 分配inode号
/// @return inode号(理论上不会失败)
static uint64_t vfs_alloc_inum()
{
        uint64_t inum;

        acquire(&inum_lk);
        inum = _vfs_inum_counter++;
        release(&inum_lk);

        return inum;
}

static void vfs_filesystem_init(void)
{
        fs_registry.head = NULL;
        initlock(&fs_registry.lock, "vfs_fs");
}

static bool vfs_in_group(const struct credentials *cred,
                         uint32_t gid)
{
        if (cred->gid == gid)
                return true;

        for (uint32_t i = 0; i < cred->ngroups; i++)
        {
                if (cred->groups[i] == gid)
                        return true;
        }

        return false;
}

/// @brief 检查当前凭据是否具有访问 inode 所需的权限。
/// @param inode       被访问的 inode
/// @param cred        当前进程权限凭据
/// @param mask        所需权限，例如 MAY_READ | MAY_WRITE
/// @return 具有所需权限返回 0，否则返回 -1
int vfs_permission(const struct inode *inode,
                   const struct credentials *cred,
                   uint32_t mask)
{
        if (inode == NULL || cred == NULL)
                return -1;

        /*
         * root 暂时视为拥有全部权限。
         */
        if (cred->uid == 0)
                return 0;

        uint32_t perm;

        if (cred->uid == inode->uid)
        {
                perm = (inode->mode >> 6) & 07;
        }
        else if (vfs_in_group(cred, inode->gid))
        {
                perm = (inode->mode >> 3) & 07;
        }
        else
        {
                perm = inode->mode & 07;
        }

        if ((mask & MAY_READ) && !(perm & 04))
                return -1;

        if ((mask & MAY_WRITE) && !(perm & 02))
                return -1;

        if ((mask & MAY_EXEC) && !(perm & 01))
                return -1;

        return 0;
}

/// @brief 在 path 指定的位置创建目录。
///
/// @param path 目录路径，例如 "/a/b"
/// @param mode 创建权限，例如 0755
///
/// @return 成功 0，失败 -1
int vfs_mkdir(const char *path,
              uint32_t mode)
{
        if (path == NULL)
                return -1;

        struct inode *parent = NULL;

        char *name = slab_alloc(VFS_MAX_PATH_LEN);

        if (name == NULL)
                return -1;

        if (vfs_lookup_parent(path, &parent, name) < 0)
        {
                slab_free(name);
                return -1;
        }

        if (parent == NULL ||
            parent->iops == NULL ||
            parent->iops->mkdir == NULL)
        {
                slab_free(name);
                return -1;
        }

        if ((parent->mode & S_IFMT) != S_IFDIR)
        {
                slab_free(name);
                return -1;
        }

        struct task_struct *task = get_task();

        /*
         * 创建目录项需要父目录具有：
         *
         * MAY_WRITE : 修改目录内容
         * MAY_EXEC  : 搜索/访问目录
         */
        if (vfs_permission(parent,
                           &task->cred,
                           MAY_WRITE | MAY_EXEC) < 0)
        {
                slab_free(name);
                return -1;
        }

        /*
         * VFS 对外只接受权限位。
         * 目录类型由 VFS 负责补上。
         */
        mode &= 0777;

        int ret = parent->iops->mkdir(parent,
                                      name,
                                      S_IFDIR | mode);

        slab_free(name);

        return ret;
}

/// @brief 删除 path 指定的目录项。
///
/// @param path 要删除的文件或目录路径
///
/// @return 成功 0，失败 -1
int vfs_unlink(const char *path)
{
        if (path == NULL)
                return -1;

        struct inode *parent = NULL;

        char *name = slab_alloc(VFS_MAX_PATH_LEN);

        if (name == NULL)
                return -1;

        if (vfs_lookup_parent(path, &parent, name) < 0)
        {
                slab_free(name);
                return -1;
        }

        if (parent == NULL ||
            parent->iops == NULL ||
            parent->iops->unlink == NULL)
        {
                slab_free(name);
                return -1;
        }

        if ((parent->mode & S_IFMT) != S_IFDIR)
        {
                slab_free(name);
                return -1;
        }

        struct task_struct *task = get_task();

        /*
         * 删除目录项需要父目录具有：
         *
         * MAY_WRITE : 修改目录内容
         * MAY_EXEC  : 搜索/访问目录
         */
        if (vfs_permission(parent,
                           &task->cred,
                           MAY_WRITE | MAY_EXEC) < 0)
        {
                slab_free(name);
                return -1;
        }

        int ret = parent->iops->unlink(parent, name);

        slab_free(name);

        return ret;
}

int vfs_rmdir(const char *path)
{
        if (path == NULL)
                return -1;

        struct inode *parent = NULL;

        char *name = slab_alloc(VFS_MAX_PATH_LEN);

        if (name == NULL)
                return -1;

        if (vfs_lookup_parent(path, &parent, name) < 0)
        {
                slab_free(name);
                return -1;
        }

        if (parent == NULL ||
            parent->iops == NULL ||
            parent->iops->rmdir == NULL)
        {
                slab_free(name);
                return -1;
        }

        if ((parent->mode & S_IFMT) != S_IFDIR)
        {
                slab_free(name);
                return -1;
        }

        struct task_struct *task = get_task();

        /*
         * 删除目录项需要父目录具有：
         *
         * MAY_WRITE : 修改目录内容
         * MAY_EXEC  : 搜索/访问目录
         */
        if (vfs_permission(parent,
                           &task->cred,
                           MAY_WRITE | MAY_EXEC) < 0)
        {
                slab_free(name);
                return -1;
        }

        int ret = parent->iops->rmdir(parent, name);

        slab_free(name);

        return ret;
}

/// @brief 打开一个文件。
///
/// @param path 文件路径
/// @param flags 打开标志
/// @return 成功返回 fd，失败返回 -1
int vfs_open(const char *path, uint32_t flags)
{
        if (path == NULL)
                return -1;

        struct task_struct *task = get_task();

        if (task == NULL)
                return -1;

        struct inode *inode = NULL;

        /*
         * O_CREAT：
         * 文件不存在时创建。
         */
        if (flags & O_CREAT)
        {
                if (vfs_lookup(path, &inode) < 0)
                {
                        uint32_t mode = 0666;

                        if (vfs_create(path, mode, &inode) < 0)
                                return -1;
                }
                else if (flags & O_EXCL)
                {
                        return -1;
                }
        }
        else
        {
                if (vfs_lookup(path, &inode) < 0)
                        return -1;
        }

        if (inode == NULL)
                return -1;

        /*
         * O_DIRECTORY 要求目标必须是目录。
         */
        if (flags & O_DIRECTORY)
        {
                if ((inode->mode & S_IFMT) != S_IFDIR)
                        return -1;
        }

        /*
         * 根据打开方式检查权限。
         */
        uint32_t access = flags & O_ACCMODE;

        uint32_t permission = 0;

        if (access == O_RDONLY)
                permission |= MAY_READ;
        else if (access == O_WRONLY)
                permission |= MAY_WRITE;
        else if (access == O_RDWR)
                permission |= MAY_READ | MAY_WRITE;

        if (vfs_permission(inode,
                           &task->cred,
                           permission) < 0)
        {
                return -1;
        }

        /*
         * O_TRUNC：
         * 这里只负责语义检查。
         *
         * 真正截断文件需要 filesystem 提供对应操作，
         * 目前 file_operations 还没有 truncate。
         */
        if (flags & O_TRUNC)
        {
                /*
                 * TODO: truncate
                 */
        }

        struct file *file = slab_alloc(sizeof(*file));

        if (file == NULL)
                return -1;

        memset(file, 0, sizeof(*file));

        file->inode = inode;
        file->pos = 0;
        file->flags = flags;
        file->refcount = 1;

        /*
         * file_operations 来自具体 inode->sb->fs->fops
         */
        file->fops = inode->fops;

        /*
         * O_APPEND：
         * 初始位置放到文件末尾。
         */
        if (flags & O_APPEND)
                file->pos = inode->size;

        /*
         * 找一个空闲 fd。
         */
        int fd = -1;

        acquire(&task->lk);

        for (int i = 0; i < NOFILE; i++)
        {
                if (task->ofile[i] == NULL)
                {
                        task->ofile[i] = file;
                        fd = i;
                        break;
                }
        }

        release(&task->lk);

        if (fd < 0)
        {
                slab_free(file);
                return -1;
        }

        return fd;
}