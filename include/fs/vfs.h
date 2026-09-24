#ifndef _INC_VFS_
#define _INC_VFS_
#include <type.h>
#include <block_device.h>
#include <sleeplock.h>
#include <hashmap.h>

#define VFS_ROOT_PARENT_SPECIAL 0xDEADBEEFCAFEBABEULL
#define VFS_STD_DEVICE "/dev/tty/uart/0"
/*
VFS inode 生命周期总结

核心原则：

inode_get = 获得一个长期持有 inode 指针的所有权

inode_put = 释放自己持有的那一个所有权

最终释放由 inode_put 触发，不由 inode_cache_remove 直接释放。

inode 引用来源

一个 inode 的 refcount：

inode.refcount

=
super_block 持有
+
inode_cache 持有
+
file 持有
+
cwd 持有
+
lookup 临时持有
+
其他内核对象持有
inode_get 场景
1. inode 创建

filesystem 创建 inode：

fat12_lookup()
	|
	v
struct inode
	|
	v
refcount = 1

这个引用属于调用者。

2. 加入 inode cache
inode_cache_insert(inode)

cache 长期保存：

refcount++

原:

inode

后:

inode
 |
 +-- cache
3. 从 cache 查找
inode_cache_find()

返回 inode 给调用者：

必须：

inode_get()

因为：

cache引用 != 调用者引用

否则调用者 put 会错误释放 cache 引用。

4. file 保存 inode

open：

vfs_lookup()
	|
	v
inode
	|
	v
file->inode

这里是：

引用转移

不是增加：

lookup引用
	|
	v
file引用

所以：

成功 open：

不用 inode_get

失败：

inode_put

5. cwd 保存 inode

例如：

task->cwd

属于长期对象：

set_cwd：

inode_get(new_cwd)

inode_put(old_cwd)
6. super_block 保存 root inode

mount：

sb
 |
 root inode

所以：

refcount++

表示：

super_block持有root

umount：

inode_put(sb->root)
inode_put 场景
lookup结束

例如：

current
 |
 v
next

切换：

inode_put(current);
current = next;

释放当前遍历引用。

file关闭
file_put()

	|
	v

inode_put(file->inode)

释放 file 持有引用。

cwd修改/进程退出
inode_put(task->cwd)

释放 cwd 引用。

cache删除
inode_cache_remove()

不是释放 inode。

只是：

删除 hashmap
释放 cache 引用

等价：

hash_table_delete();

inode_put(inode);
*/
// 默认128个inode
#define VFS_INODE_CACHE_SIZE 128
#define VFS_NAME_MAX 255

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define MAY_READ 0x01
#define MAY_WRITE 0x02
#define MAY_EXEC 0x04

/*
 * inode mode
 *
 * 高位：文件类型
 * 低位：文件权限
 */

/* file type */
#define S_IFMT 0170000
#define S_IFREG 0100000 /* regular file */
#define S_IFDIR 0040000 /* directory */
#define S_IFCHR 0020000 /* character device */
#define S_IFBLK 0060000 /* block device */
#define S_IFLNK 0120000 /* symbolic link */

/* owner permission */
#define S_IRUSR 0000400
#define S_IWUSR 0000200
#define S_IXUSR 0000100

/* group permission */
#define S_IRGRP 0000040
#define S_IWGRP 0000020
#define S_IXGRP 0000010

/* other permission */
#define S_IROTH 0000004
#define S_IWOTH 0000002
#define S_IXOTH 0000001

#define S_IRWXU (S_IRUSR | S_IWUSR | S_IXUSR)
#define S_IRWXG (S_IRGRP | S_IWGRP | S_IXGRP)
#define S_IRWXO (S_IROTH | S_IWOTH | S_IXOTH)

/* access mode */
#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR 0x0002

#define O_ACCMODE 0x0003

/* open behavior */
#define O_CREAT 0x0004
#define O_EXCL 0x0008
#define O_TRUNC 0x0010
#define O_APPEND 0x0020
#define O_DIRECTORY 0x0040

#define VFS_FS_NAME_MAX 64
#define VFS_MAX_PATH_LEN 128 // 最长就是128的path描述
#define VFS_MAX_FD_NUM 64

struct filesystem;
struct super_block;
struct mount;
struct inode;
struct file;
struct file_operations;

struct filesystem_registry {
	struct filesystem* head;

	struct spinlock lock;
};

typedef struct filesystem_registry filesystem_registry_t;

/// @note fs和sb的关系在于: 一个是描述数据的组织方式,
/// 一个是描述该以什么样的方式读取
/*
 * 文件系统类型
 */
struct filesystem {
	const char name[VFS_FS_NAME_MAX];

	int (*get_super)(struct filesystem* fs,
			 struct block_device* dev,
			 struct super_block** sb);

	void (*kill_sb)(struct super_block* sb);

	struct file_operations* fops;
	struct inode_operations* iops;

	struct filesystem* next;
};

typedef struct filesystem filesystem_t;

struct vfs_kstat {
	uint64_t ino;
	uint32_t mode;

	uint32_t uid;
	uint32_t gid;
	uint64_t size;
	uint32_t nlink;
	uint64_t blocks;
	uint64_t blksize;
	uint64_t atime;
	uint64_t mtime;
	uint64_t ctime;
};

struct vfs_dirent {
	uint64_t ino;

	uint32_t type;

	char name[VFS_NAME_MAX];
};

/*
 * 一个具体的挂载点
 * /media/loe -> 某个 FAT12 super_block
 */
struct mount {
	char path[VFS_MAX_PATH_LEN];

	struct super_block* sb;

	struct mount* parent;
	struct mount* child;
	struct mount* next;
};

typedef struct mount mount_t;

struct mount_table {
	struct mount* root;

	struct spinlock lock;
};

typedef struct mount_table mount_table_t;

/*
 * 一个具体的文件系统实例
 *
 * 如：loeux.img 被作为 FAT12 挂载到 /media/loe
 * 具体文件系统实现时，super_block应持有一个inode的引用
 */
struct super_block {
	struct filesystem* fs;
	struct block_device* dev;

	struct inode* root;

	void* private;
};

typedef struct super_block super_block_t;

/*
	锁序：
		icache.lock
		     ↓
		inode->lock
*/
struct inode_cache {
	hash_table_t* table;
	struct spinlock lock;
};
typedef struct inode_cache inode_cache_t;

struct inode_cache_key {
	struct inode* parent;
	char name[VFS_MAX_PATH_LEN];
};
typedef struct inode_cache_key inode_cache_key_t;

struct inode {
	struct super_block* sb;

	uint64_t ino;

	uint32_t mode;

	uint32_t uid;
	uint32_t gid;

	uint64_t size;

	struct inode_operations* iops;
	struct file_operations* fops;

	void* private;

	uint32_t refcount;

	struct inode_cache_key cache_key;

	spinlock_t lock;
};

typedef struct inode inode_t;

/*
 * 一次 open 对应的 open file description
 *
 * fork / dup 后可以被多个 fd 共享。
 */
struct file {
	struct inode* inode;

	uint64_t pos;
	uint32_t flags;

	struct file_operations* fops;

	void* private;

	uint32_t refcount;

	struct sleeplock slk;
};

typedef struct file file_t;

/*
 * 进程自己的 fd 表
 */
struct fd_table {
	struct file* files[VFS_MAX_FD_NUM];

	struct spinlock lock;
};

/*
 * inode 层操作
 *
 * 操作“文件 / 目录对象本身”
 */
struct inode_operations {
	int (*lookup)(struct inode* dir,
		      const char* name,
		      struct inode** inode);

	int (*create)(struct inode* dir,
		      const char* name,
		      uint32_t mode,
		      struct inode** inode);

	int (*mkdir)(struct inode* dir,
		     const char* name,
		     uint32_t mode,
		     struct inode** inode);

	int (*rmdir)(struct inode* dir, const char* name);

	int (*unlink)(struct inode* dir, const char* name);

	int (*readdir)(struct inode* dir,
		       uint64_t* offset,
		       struct vfs_dirent* dirent);

	int (*getattr)(struct inode* inode, struct vfs_kstat* stat);

	int (*truncate)(struct inode* inode, uint64_t size);

	void (*destroy)(struct inode* inode);
};

/*
 * file 层操作
 *
 * 操作“已经 open 的文件”
 * 对于file 和 file_operations:
 * process
 * │
 * └── fd_table[3]
 *         │
 *         ▼
 *      file
 *      ├── inode ─────► hello.txt
 *      ├── pos = 0
 *      ├── flags
 *      └── fops ──────► fat12_file_ops
 *                             │
 *                             ├── read  → fat12_read
 *                             ├── write → fat12_write
 *                             ├── seek  → fat12_seek
 *                             └── close → fat12_close
 */
struct file_operations {
	int64_t (*read)(struct file* file, void* buf, uint64_t count);

	int64_t (*write)(struct file* file, const void* buf, uint64_t count);

	int64_t (*seek)(struct file* file, int64_t offset, int whence);

	int (*close)(struct file* file);
};

typedef struct inode_operations inode_operations_t;
typedef struct file_operations file_operations_t;

/// @brief 初始化 VFS 子系统。
///
/// 初始化 inode 号分配锁、挂载表锁、文件系统注册表以及 inode cache。
void init_vfs(void);

/// @brief 挂载一个文件系统。
///
/// 在 target 指定的挂载点挂载 fs_type 类型的文件系统。
/// 若 target 为 "/"，则作为根文件系统挂载。
///
/// @param dev     块设备
/// @param target  挂载点路径，例如 "/"、"/mnt"
/// @param fs_type 文件系统类型名，例如 "fat32"
///
/// @return 成功返回 0，失败返回 -1
int vfs_mount(struct block_device* dev,
	      const char* target,
	      const char* fs_type);

/// @brief 卸载挂载点 target。
///
/// 拒绝卸载根文件系统；有子挂载点、或任一进程的
/// ofile / cwd 仍引用该文件系统时返回失败（busy）。
///
/// @param target 挂载点路径，例如 "/mnt"
///
/// @return 成功返回 0，失败返回 -1
int vfs_umount(const char* target);

/// @brief 返回根挂载点。
///
/// 用于调试（如打印挂载表）。未挂载根文件系统时返回 NULL。
///
/// @return 根挂载点，失败返回 NULL
struct mount* vfs_get_root_mount(void);

/// @brief 为当前进程建立标准输入/输出/错误（fd 0/1/2）。
///
/// 三个 fd 都指向控制台字符设备 /dev/ttyS0。
/// 必须在 ofile[] 全空的进程上调用。
void init_vfs_std(void);

/// @brief 给定一个绝对路径，找到覆盖该路径的最深层 mount。
///
/// @param path 绝对路径
///
/// @return 覆盖该路径的最深挂载点，失败返回 NULL
struct mount* vfs_find_mount(const char* path);

/// @brief 注册一个文件系统到文件系统注册表。
///
/// fs 对象的生命周期在运行期间不会被销毁。
///
/// @param fs 文件系统对象
///
/// @return 成功返回 0，失败返回 -1
int vfs_register_filesystem(struct filesystem* fs);

/// @brief 根据名字查找已注册的文件系统。
///
/// @param name 文件系统类型名
///
/// @return 找到返回 filesystem，失败返回 NULL
struct filesystem* vfs_get_filesystem(const char* name);

/// @brief 解析绝对路径，返回对应 inode。
///
/// 会优先查 inode cache，未命中时进入具体文件系统的 lookup。
/// 返回的 inode 带有一个调用者引用。
///
/// @param path  绝对路径
/// @param inode 返回解析到的 inode
///
/// @return 成功返回 0，失败返回 -1
int vfs_lookup(const char* path, struct inode** inode);

/// @brief 解析 path，找到其直接父目录 inode，
///        并将最后一个路径组件写入 name。
///
/// 父目录不存在或路径非法则失败。
/// 返回的 parent 带有一个调用者引用。
///
/// @param path   文件路径
/// @param parent 返回父目录 inode
/// @param name   返回最后一级组件名称
///
/// @return 成功返回 0，失败返回 -1
int vfs_lookup_parent(const char* path, struct inode** parent, char* name);

/// @brief 检查当前凭据是否具有访问 inode 所需的权限。
///
/// @param inode 被访问的 inode
/// @param cred  当前进程权限凭据
/// @param mask  所需权限，例如 MAY_READ | MAY_WRITE
///
/// @return 具有所需权限返回 0，否则返回 -1
int vfs_permission(const struct inode* inode,
		   const struct credentials* cred,
		   uint32_t mask);

/// @brief 在 path 指定的位置创建普通文件，并返回新文件 inode。
///
/// @param path  文件路径
/// @param mode  创建权限位，仅包含 rwx 权限
/// @param inode 返回创建后的 inode
///
/// @return 成功返回 0，失败返回 -1
int vfs_create(const char* path, uint32_t mode, struct inode** inode);

/// @brief 在 path 指定的位置创建目录。
///
/// @param path 目录路径，例如 "/a/b"
/// @param mode 创建权限，仅包含 rwx 权限位
///
/// @return 成功返回 0，失败返回 -1
int vfs_mkdir(const char* path, uint32_t mode);

/// @brief 删除 path 指定的普通文件目录项。
///
/// @param path 文件路径
///
/// @return 成功返回 0，失败返回 -1
int vfs_unlink(const char* path);

/// @brief 删除 path 指定的目录。
///
/// @param path 目录路径
///
/// @return 成功返回 0，失败返回 -1
int vfs_rmdir(const char* path);

/// @brief 根据父目录 inode 和文件名查找缓存中的 inode。
///
/// @param parent 父目录 inode
/// @param name   文件名
///
/// @return 命中的 inode，未找到时返回 NULL
struct inode* inode_cache_find(struct inode* parent, const char* name);

/// @brief 将 inode 插入 inode cache。
///
/// @param inode 要缓存的 inode
///
/// @return 成功返回 0，inode 已存在或参数非法时返回 -1
int inode_cache_insert(struct inode* inode);

/// @brief 从 inode cache 中移除 inode。
///
/// 仅当 inode 只由 cache 持有时才会真正移除并释放。
///
/// @param inode 要移除的 inode
void inode_cache_remove(struct inode* inode);

/// @brief 增加 inode 的引用计数。
///
/// @param inode 要增加引用计数的 inode
void inode_get(struct inode* inode);

/// @brief 减少 inode 的引用计数，不会直接 free。
///
/// @param inode 要减少引用计数的 inode
void inode_put(struct inode* inode);

/// @brief 打开一个文件。
///
/// 支持 O_CREAT、O_EXCL、O_DIRECTORY、O_TRUNC、O_APPEND 等标志。
///
/// @param path  文件路径
/// @param flags 打开标志
///
/// @return 成功返回 fd，失败返回 -1
int vfs_open(const char* path, uint32_t flags);

/// @brief 关闭一个文件对象。
///
/// 内部调用 file_put，减少引用计数。
///
/// @param file 要关闭的文件对象
void vfs_close(struct file* file);

/// @brief 复制一个文件描述符。
///
/// @param oldfd 原文件描述符
///
/// @return 成功返回新的 fd，失败返回 -1
int vfs_dup(int oldfd);

/// @brief 增加 file 的引用计数。
///
/// @param file 目标 file
void file_get(struct file* file);

/// @brief 减少 file 的引用计数。
///
/// 引用计数归零时调用 fops->close，释放 inode 引用并释放 file。
///
/// @param file 目标 file
void file_put(struct file* file);

/// @brief 为 file 分配一个文件描述符。
///
/// 成功后 fd 持有一个 file 引用。
///
/// @param file 要分配 fd 的文件对象
///
/// @return 成功返回 fd，失败返回 -1
int fd_alloc(struct file* file);

/// @brief 根据 fd 获取当前进程打开文件。
///
/// @param fd 文件描述符
///
/// @return 成功返回 file，并增加引用；失败返回 NULL
struct file* fd_get(int fd);

/// @brief 关闭一个文件描述符。
///
/// 解除 fd 对 file 的引用，并释放 fd 持有的引用。
///
/// @param fd 文件描述符
///
/// @return 成功返回 0，失败返回 -1
int fd_close(int fd);

/// @brief 从打开文件中读取数据。
///
/// 从 file 当前偏移位置读取最多 count 字节到 buf，
/// 成功后更新 file->pos。
///
/// @param file  打开的文件对象
/// @param buf   输出缓冲区
/// @param count 期望读取的字节数
///
/// @return 成功返回实际读取的字节数，失败返回 -1
int64_t vfs_read(struct file* file, void* buf, uint64_t count);

/// @brief 向打开文件中写入数据。
///
/// 从 file 当前偏移位置写入最多 count 字节，
/// 成功后更新 file->pos。
///
/// @param file  打开的文件对象
/// @param buf   输入缓冲区
/// @param count 期望写入的字节数
///
/// @return 成功返回实际写入的字节数，失败返回 -1
int64_t vfs_write(struct file* file, const void* buf, uint64_t count);

/// @brief 修改打开文件的当前偏移位置。
///
/// @param file   打开的文件对象
/// @param offset 偏移量
/// @param whence 偏移基准
///                SEEK_SET  从文件开始
///                SEEK_CUR  从当前位置
///                SEEK_END  从文件末尾
///
/// @return 成功返回新的文件位置，失败返回 -1
int64_t vfs_seek(struct file* file, int64_t offset, int whence);

/// @brief 获取打开文件的属性。
///
/// @param file 打开的文件
/// @param stat 属性结构
///
/// @return 成功 0，失败 -1
int vfs_fstat(struct file* file, struct vfs_kstat* stat);

/// @brief 读取目录中的下一项。
///
/// 只有目录类型的 inode 才能 readdir。
///
/// @param file   打开的目录文件
/// @param dirent 输出目录项
///
/// @return 成功返回 0，结束或失败返回 -1
int vfs_readdir(struct file* file, struct vfs_dirent* dirent);

/// @brief 截断 inode 对应文件大小。
///
/// 将 inode 对应文件的大小修改为 size。
/// 调用者需要对该 inode 具有写权限。
///
/// @param inode 目标 inode
/// @param size  新文件大小
///
/// @return 成功返回 0，失败返回 -1
int vfs_truncate(struct inode* inode, uint64_t size);

/// @brief 根据 fd 修改打开文件的当前偏移位置。
///
/// 内部通过 fd_get 获取 file，调用 vfs_seek 后释放临时引用。
///
/// @param fd     文件描述符
/// @param offset 偏移量
/// @param whence 偏移基准
///                SEEK_SET  从文件开始
///                SEEK_CUR  从当前位置
///                SEEK_END  从文件末尾
///
/// @return 成功返回新的文件位置，失败返回 -1
int64_t vfs_seek_fd(int fd, int64_t offset, int whence);

#endif
