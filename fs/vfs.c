#include <vfs.h>
#include <panic.h>
#include <sleeplock.h>
#include <slab.h>
#include <spinlock.h>
#include <proc.h>
#include <memory.h>
#include <fat32.h>
#include <block_device.h>
#include <char_dev.h>
#include <lib.h>
#include <syscall.h>

/*
 * VFS 层开发范式：
 *   1. 路径规范化与父目录解析
 *   2. 权限校验——调用者必须对父目录具有 MAY_WRITE | MAY_EXEC
 *   3. 并发保护——通过 vfs_meta_lock 序列化元数据修改
 *   4. 权限位设置——文件系统层只负责填写文件类型（S_IFREG），
 *      rwx 权限位由 VFS 层覆盖到 inode->mode
 *   5. inode cache 维护
 *
 *   @author loehash 26-09-17
 */

/*
inode.refcount
=
cache引用
+
当前调用者引用
+
file引用
+
cwd引用
+
其他内核对象引用
*/

static mount_table_t mount_table = {0};
static filesystem_registry_t fs_registry = {0};
static void vfs_filesystem_init(void);
static void vfs_inode_cache_init(void);
static inode_cache_t icache = {0};

/*
 * 全局元数据操作锁
 *
 * 设计：
 *   文件系统层(fat32等)只负责磁盘读写，认为自身是并发安全的，
 *   不在文件系统层内部做并发互斥。所有多进程并发写防护统一在 VFS
 *   层完成。此锁序列化所有元数据修改操作（create / mkdir / unlink /
 *   rmdir / truncate），防止竞态导致目录项损坏或簇链断裂。
 *
 *   read / write / seek / readdir 不需要此锁——它们由 file->slk
 *   保护同一 file 对象的并发访问。
 */
static struct sleeplock vfs_meta_lock;
static int vfs_meta_lock_inited = 0;

static uint32_t inode_cache_hash(const void* key, uint32_t key_len);
static bool
inode_cache_key_equal(const void* key1, const void* key2, uint32_t key_len);

void init_vfs()
{
	init_spinlock(&mount_table.lock);

	vfs_filesystem_init();
	vfs_inode_cache_init();

	init_sleeplock(&vfs_meta_lock);
	vfs_meta_lock_inited = 1;
}

int vfs_mount(struct block_device* dev, const char* target, const char* fs_type)
{
	struct filesystem* fs = NULL;
	struct super_block* sb = NULL;
	struct mount* mnt = NULL;
	struct mount* parent = NULL;

	if (dev == NULL || target == NULL || fs_type == NULL)
		return -1;

	fs = vfs_get_filesystem(fs_type);

	if (fs == NULL)
		return -1;

	if (fs->get_super(fs, dev, &sb) < 0)
		return -1;

	if (sb == NULL || sb->root == NULL) {
		if (sb != NULL)
			fs->kill_sb(sb);
		return -1;
	}

	/*
	 * 将super_block提供的root inode加入inode cache
	 *
	 * 根挂载：cache_key.parent = SPECIAL，cache 不持有父引用
	 * 非根挂载：cache_key.parent = 挂载点目录，cache 持有挂载点的引用
	 */
	if (strcmp(target, "/") == 0) {
		sb->root->cache_key.parent =
		    (struct inode*)VFS_ROOT_PARENT_SPECIAL;
	} else {
		parent = vfs_find_mount(target);

		if (parent == NULL || parent->sb == NULL ||
		    parent->sb->root == NULL) {
			fs->kill_sb(sb);
			return -1;
		}

		sb->root->cache_key.parent = parent->sb->root;

		/* cache 持有挂载点父目录引用 */
		inode_get(parent->sb->root);
	}

	strcpy(sb->root->cache_key.name, "/");

	if (inode_cache_insert(sb->root) < 0) {
		if (parent != NULL)
			inode_put(parent->sb->root);

		fs->kill_sb(sb);
		return -1;
	}

	mnt = slab_alloc(sizeof(*mnt));

	if (mnt == NULL) {
		/* 释放 cache 引用与挂载点父引用 */
		inode_cache_remove(sb->root);

		fs->kill_sb(sb);
		return -1;
	}

	memset(mnt, 0, sizeof(*mnt));

	strcpy(mnt->path, target);

	mnt->sb = sb;

	acquire(&mount_table.lock);

	if (strcmp(target, "/") == 0) {
		if (mount_table.root != NULL) {
			release(&mount_table.lock);

			inode_cache_remove(sb->root);
			fs->kill_sb(sb);
			slab_free(mnt);
			return -1;
		}

		mount_table.root = mnt;

		release(&mount_table.lock);

		return 0;
	}

	/*
	 * 挂到父挂载点的孩子链表头部
	 */
	mnt->parent = parent;

	mnt->next = parent->child;
	parent->child = mnt;

	release(&mount_table.lock);

	return 0;
}

/// @brief 在 path 指定的位置创建普通文件，并返回新文件 inode。
/// @param path 文件路径
/// @param mode 创建权限位，仅包含 rwx 权限
/// @param inode 返回创建后的 inode
/// @return 成功返回0，失败返回-1
int vfs_create(const char* path, uint32_t mode, struct inode** inode)
{
	if (path == NULL || inode == NULL)
		return -1;

	struct inode* parent = NULL;
	char* name = slab_alloc(VFS_MAX_PATH_LEN);

	if (name == NULL)
		return -1;

	if (vfs_lookup_parent(path, &parent, name) < 0) {
		slab_free(name);
		return -1;
	}

	if (parent == NULL || parent->iops == NULL ||
	    parent->iops->create == NULL) {
		if (parent)
			inode_put(parent);

		slab_free(name);
		return -1;
	}

	if ((parent->mode & S_IFMT) != S_IFDIR) {
		inode_put(parent);
		slab_free(name);
		return -1;
	}

	struct task_struct* task = get_task();

	/*
	 * 权限校验（VFS 层拦截）：
	 * 修改目录内容需要 MAY_WRITE，搜索目录需要 MAY_EXEC。
	 */
	if (vfs_permission(parent, &task->cred, MAY_WRITE | MAY_EXEC) < 0) {
		inode_put(parent);
		slab_free(name);
		return -1;
	}

	mode &= 0777;

	/*
	 * 并发保护：序列化元数据修改，防止多进程同时
	 * 分配同一目录项或损坏簇链。
	 */
	acquire_sleep(&vfs_meta_lock);

	/*
	 * 二次检查缓存：防止 lookup_parent 后、获取锁前
	 * 其他进程已创建同名文件。
	 */
	{
		struct inode* exist = inode_cache_find(parent, name);
		if (exist != NULL) {
			inode_put(exist);
			release_sleep(&vfs_meta_lock);
			inode_put(parent);
			slab_free(name);
			return -1;
		}
	}

	struct inode* new_inode = NULL;

	if (parent->iops->create(parent, name, S_IFREG | mode, &new_inode) <
		0 ||
	    new_inode == NULL) {
		release_sleep(&vfs_meta_lock);
		inode_put(parent);
		slab_free(name);
		return -1;
	}

	/*
	 * 权限位由 VFS 层统一设置（开发范式）：
	 * 文件系统层只填写 S_IFREG，rwx 权限由 VFS 覆盖。
	 */
	new_inode->mode = (new_inode->mode & S_IFMT) | mode;
	new_inode->uid = task->cred.uid;
	new_inode->gid = task->cred.gid;

	new_inode->cache_key.parent = parent;

	strcpy(new_inode->cache_key.name, name);

	/* cache 持有 parent 引用 */
	inode_get(parent);

	if (inode_cache_insert(new_inode) < 0) {
		/* cache 未接管，释放刚取的 parent 引用 */
		inode_put(parent);

		inode_put(new_inode);

		/* 释放 lookup_parent 返回的 caller 引用 */
		inode_put(parent);
		release_sleep(&vfs_meta_lock);
		slab_free(name);
		return -1;
	}

	// 给调用者增加一个引用
	inode_get(new_inode);

	// 释放filesystem创建时返回的引用
	inode_put(new_inode);

	*inode = new_inode;

	inode_put(parent);

	release_sleep(&vfs_meta_lock);

	slab_free(name);

	return 0;
}

/// @brief 文本层路径规范化。
///
/// FAT 等 VFS 后端的目录里不存在 "." / ".." 目录项，
/// 相对路径与 "." ".." "//" 必须在进入 inode walk 前消解。
/// 复用 do_build_user_path：拼接 cwd、消解 "." ".." "//"、去尾 '/'。
/// @param path 原始路径（相对或绝对）
/// @param out  输出缓冲区，调用者负责分配 BUFSZ 字节
/// @return 0 成功（out 为绝对路径），-1 失败（超长等得到空串）
static int vfs_normalize(const char* path, char* out)
{
	struct task_struct* ts = get_task();
	const char* cwd;

	if (path == NULL || out == NULL)
		return -1;

	/*
	 * 无任务上下文（早期启动）或 cwd 尚未设置：
	 * 仅绝对路径可解析；cwd 传 "/" 仅为满足
	 * do_build_user_path 的前置检查。
	 */
	if (ts == NULL || ts->cwd_path[0] == '\0')
		cwd = "/";
	else
		cwd = ts->cwd_path;

	do_build_user_path(out, (char*)path, (char*)cwd);

	/* 结果必须能放进 VFS 各路径缓冲（VFS_MAX_PATH_LEN） */
	if (out[0] == '\0' || strlen(out) >= VFS_MAX_PATH_LEN)
		return -1;

	return 0;
}

static int vfs_lookup_impl(const char* path, struct inode** inode)
{
	if (path == NULL || inode == NULL)
		return -1;

	struct inode* current = NULL;
	const char* relative = NULL;

	if (path[0] == '/') {
		struct mount* mnt = vfs_find_mount(path);

		if (mnt == NULL || mnt->sb == NULL || mnt->sb->root == NULL)
			return -1;

		current = mnt->sb->root;
		relative = path + strlen(mnt->path);

		if (*relative == '/')
			relative++;
	} else {
		struct task_struct* task = get_task();

		if (task == NULL || task->cwd == NULL)
			return -1;

		current = task->cwd;
		relative = path;
	}

	inode_get(current);

	if (*relative == '\0') {
		*inode = current;
		return 0;
	}

	char* name = slab_alloc(VFS_MAX_PATH_LEN);

	if (name == NULL) {
		inode_put(current);
		return -1;
	}

	struct task_struct* task = get_task();

	if (task == NULL) {
		slab_free(name);
		inode_put(current);
		return -1;
	}

	while (*relative != '\0') {
		const char* slash = strchr(relative, '/');
		uint64_t len;

		if (slash != NULL)
			len = slash - relative;
		else
			len = strlen(relative);

		if (len == 0) {
			relative++;
			continue;
		}

		if (len >= VFS_MAX_PATH_LEN) {
			slab_free(name);
			inode_put(current);
			return -1;
		}

		memcpy(name, relative, len);
		name[len] = '\0';

		if (vfs_permission(current, &task->cred, MAY_EXEC) < 0) {
			slab_free(name);
			inode_put(current);
			return -1;
		}

		struct inode* next = inode_cache_find(current, name);

		if (next == NULL) {
			if (current->iops == NULL ||
			    current->iops->lookup == NULL) {
				slab_free(name);
				inode_put(current);
				return -1;
			}

			if (current->iops->lookup(current, name, &next) < 0 ||
			    next == NULL) {
				slab_free(name);
				inode_put(current);
				return -1;
			}

			next->cache_key.parent = current;

			strcpy(next->cache_key.name, name);

			// cache持有parent引用
			inode_get(current);

			if (inode_cache_insert(next) < 0) {
				inode_put(current);
				inode_put(next);

				slab_free(name);
				inode_put(current);
				return -1;
			}
		}

		inode_put(current);

		current = next;

		relative += len;

		if (*relative == '/')
			relative++;
	}

	slab_free(name);

	*inode = current;

	return 0;
}

int vfs_lookup(const char* path, struct inode** inode)
{
	char* nbuf;
	int ret;

	if (path == NULL || inode == NULL)
		return -1;

	nbuf = slab_alloc(BUFSZ);

	if (nbuf == NULL)
		return -1;

	if (vfs_normalize(path, nbuf) < 0) {
		slab_free(nbuf);
		return -1;
	}

	ret = vfs_lookup_impl(nbuf, inode);

	slab_free(nbuf);

	return ret;
}

struct filesystem* vfs_get_filesystem(const char* name)
{
	if (name == NULL)
		return NULL;

	acquire(&fs_registry.lock);

	struct filesystem* fs = fs_registry.head;

	while (fs != NULL) {
		if (strcmp(fs->name, name) == 0)
			break;

		fs = fs->next;
	}

	release(&fs_registry.lock);

	return fs;
}

/// @brief 挂载点路径前缀匹配。
///
/// "/mnt" 匹配 "/mnt" 与 "/mnt/x"，但不匹配 "/mntx"。
static int mount_path_match(const char* mnt_path, const char* path)
{
	size_t len = strlen(mnt_path);

	if (strncmp(mnt_path, path, len) != 0)
		return 0;

	if (path[len] == '\0' || path[len] == '/')
		return 1;

	return 0;
}

/// @brief 给定一个绝对路径 path，找到 覆盖该路径的最深层 mount。
struct mount* vfs_find_mount(const char* path)
{
	if (path == NULL || path[0] != '/')
		return NULL;

	struct mount* mnt = mount_table.root;

	if (mnt == NULL)
		return NULL;

	while (1) {
		struct mount* child = mnt->child;

		while (child != NULL) {
			if (mount_path_match(child->path, path))
				break;

			child = child->next;
		}

		if (child == NULL)
			return mnt;

		mnt = child;
	}
}

/// @brief 解析 path，找到其直接父目录 inode
///        并将最后一个路径组件写入 name。
///        父目录不存在或路径非法则失败。
/// @param path 文件路径
/// @param parent 返回父目录 inode
/// @param name 返回最后一级组件名称
/// @return 成功返回0，失败返回-1
static int
vfs_lookup_parent_impl(const char* path, struct inode** parent, char* name)
{
	if (path == NULL || parent == NULL || name == NULL) {
		return -1;
	}

	if (path[0] != '/')
		return -1;

	size_t len = strlen(path);

	/*
	 * 根目录不能创建
	 *
	 * /
	 */
	if (len <= 1)
		return -1;

	/*
	 * 去除末尾 '/'
	 *
	 * 防止：
	 *
	 * /a/b/
	 */
	while (len > 1 && path[len - 1] == '/') {
		len--;
	}

	/*
	 * 找最后一个 '/'
	 */
	const char* slash = NULL;

	for (int i = len - 1; i >= 0; i--) {
		if (path[i] == '/') {
			slash = &path[i];
			break;
		}
	}

	if (slash == NULL)
		return -1;

	/*
	 * 最后一级名字
	 */
	const char* last = slash + 1;

	size_t name_len = len - (last - path);

	if (name_len == 0 || name_len >= VFS_MAX_PATH_LEN) {
		return -1;
	}

	memcpy(name, last, name_len);

	name[name_len] = '\0';

	/*
	 * 父路径长度
	 */
	size_t parent_len = slash - path;

	char* parent_path = slab_alloc(VFS_MAX_PATH_LEN);

	if (parent_path == NULL)
		return -1;

	/*
	 * 父目录是根目录
	 *
	 * /a
	 *
	 * parent = /
	 */
	if (parent_len == 0) {
		parent_path[0] = '/';
		parent_path[1] = '\0';
	} else {
		if (parent_len >= VFS_MAX_PATH_LEN) {
			slab_free(parent_path);
			return -1;
		}

		memcpy(parent_path, path, parent_len);

		parent_path[parent_len] = '\0';
	}

	/*
	 * vfs_lookup:
	 *
	 * 负责 inode cache
	 *
	 * 返回 parent inode 引用
	 */
	int ret = vfs_lookup(parent_path, parent);

	slab_free(parent_path);

	return ret;
}

int vfs_lookup_parent(const char* path, struct inode** parent, char* name)
{
	char* nbuf;
	int ret;

	if (path == NULL || parent == NULL || name == NULL) {
		return -1;
	}

	nbuf = slab_alloc(BUFSZ);

	if (nbuf == NULL)
		return -1;

	if (vfs_normalize(path, nbuf) < 0) {
		slab_free(nbuf);
		return -1;
	}

	ret = vfs_lookup_parent_impl(nbuf, parent, name);

	slab_free(nbuf);

	return ret;
}

// fs对象的生命周期在运行期间是不会被销毁的
/// @brief 注册fs到fs注册表中
/// 0成功，其他错误
int vfs_register_filesystem(struct filesystem* fs)
{
	if (fs == NULL)
		return -1;

	acquire(&fs_registry.lock);

	// 检查同名 filesystem
	struct filesystem* p = fs_registry.head;

	while (p != NULL) {
		if (strcmp(p->name, fs->name) == 0) {
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

static void vfs_filesystem_init(void)
{
	fs_registry.head = NULL;
	init_spinlock(&fs_registry.lock);
}

static bool vfs_in_group(const struct credentials* cred, uint32_t gid)
{
	if (cred->gid == gid)
		return true;

	for (uint32_t i = 0; i < cred->ngroups; i++) {
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
int vfs_permission(const struct inode* inode,
		   const struct credentials* cred,
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

	if (cred->uid == inode->uid) {
		perm = (inode->mode >> 6) & 07;
	} else if (vfs_in_group(cred, inode->gid)) {
		perm = (inode->mode >> 3) & 07;
	} else {
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
/// VFS 层职责（开发范式）：
///   1. 路径规范化与父目录解析
///   2. 权限校验——调用者必须对父目录具有 MAY_WRITE | MAY_EXEC
///   3. 并发保护——通过 vfs_meta_lock 序列化元数据修改
///   4. 权限位设置——文件系统层只负责填写文件类型（S_IFDIR），
///      rwx 权限位由 VFS 层覆盖到 inode->mode
///   5. inode cache 维护
///
/// @param path 目录路径，例如 "/a/b"
/// @param mode 创建权限，仅包含 rwx 权限位
/// @return 成功返回0，失败返回-1
int vfs_mkdir(const char* path, uint32_t mode)
{
	if (path == NULL)
		return -1;

	struct inode* parent = NULL;
	char* name = slab_alloc(VFS_MAX_PATH_LEN);

	if (name == NULL)
		return -1;

	if (vfs_lookup_parent(path, &parent, name) < 0) {
		slab_free(name);
		return -1;
	}

	if (parent == NULL || parent->iops == NULL ||
	    parent->iops->mkdir == NULL) {
		if (parent != NULL)
			inode_put(parent);

		slab_free(name);
		return -1;
	}

	if ((parent->mode & S_IFMT) != S_IFDIR) {
		inode_put(parent);
		slab_free(name);
		return -1;
	}

	struct task_struct* task = get_task();

	/*
	 * 权限校验（VFS 层拦截）：
	 * 修改目录内容需要 MAY_WRITE，搜索目录需要 MAY_EXEC。
	 */
	if (vfs_permission(parent, &task->cred, MAY_WRITE | MAY_EXEC) < 0) {
		inode_put(parent);
		slab_free(name);
		return -1;
	}

	mode &= 0777;

	/*
	 * 并发保护：序列化元数据修改。
	 */
	acquire_sleep(&vfs_meta_lock);

	/*
	 * 二次检查缓存：防止 lookup_parent 后、获取锁前
	 * 其他进程已创建同名目录。
	 */
	{
		struct inode* exist = inode_cache_find(parent, name);
		if (exist != NULL) {
			inode_put(exist);
			release_sleep(&vfs_meta_lock);
			inode_put(parent);
			slab_free(name);
			return -1;
		}
	}

	struct inode* new_inode = NULL;

	int ret = parent->iops->mkdir(parent, name, S_IFDIR | mode, &new_inode);

	if (ret < 0 || new_inode == NULL) {
		release_sleep(&vfs_meta_lock);
		inode_put(parent);
		slab_free(name);
		return -1;
	}

	/*
	 * 权限位由 VFS 层统一设置（开发范式）：
	 * 文件系统层只填写 S_IFDIR，rwx 权限由 VFS 覆盖。
	 */
	new_inode->mode = (new_inode->mode & S_IFMT) | mode;
	new_inode->uid = task->cred.uid;
	new_inode->gid = task->cred.gid;

	new_inode->cache_key.parent = parent;

	memcpy(new_inode->cache_key.name, name, strlen(name) + 1);

	/* cache 持有 parent 引用 */
	inode_get(parent);

	if (inode_cache_insert(new_inode) < 0) {
		/* cache 未接管，释放刚取的 parent 引用 */
		inode_put(parent);

		inode_put(new_inode);

		/* 释放 lookup_parent 返回的 caller 引用 */
		inode_put(parent);
		release_sleep(&vfs_meta_lock);
		slab_free(name);
		return -1;
	}

	inode_put(new_inode);

	inode_put(parent);

	release_sleep(&vfs_meta_lock);

	slab_free(name);

	return 0;
}

/// @brief 删除 path 指定的普通文件目录项。
/// @param path 文件路径
/// @return 成功0，失败-1
/// @brief 删除 path 指定的普通文件目录项。
/// @param path 文件路径
/// @return 成功0，失败-1
int vfs_unlink(const char* path)
{
	if (path == NULL)
		return -1;

	struct inode* parent = NULL;

	char* name = slab_alloc(VFS_MAX_PATH_LEN);

	if (name == NULL)
		return -1;

	if (vfs_lookup_parent(path, &parent, name) < 0) {
		slab_free(name);
		return -1;
	}

	if (parent == NULL || parent->iops == NULL ||
	    parent->iops->unlink == NULL) {
		if (parent)
			inode_put(parent);

		slab_free(name);
		return -1;
	}

	if ((parent->mode & S_IFMT) != S_IFDIR) {
		inode_put(parent);
		slab_free(name);
		return -1;
	}

	struct task_struct* task = get_task();

	if (vfs_permission(parent, &task->cred, MAY_WRITE | MAY_EXEC) < 0) {
		inode_put(parent);
		slab_free(name);
		return -1;
	}

	/*
	 * 并发保护：序列化元数据修改。
	 */
	acquire_sleep(&vfs_meta_lock);

	struct inode* inode = inode_cache_find(parent, name);

	if (inode == NULL) {
		release_sleep(&vfs_meta_lock);
		inode_put(parent);
		slab_free(name);
		return -1;
	}

	if ((inode->mode & S_IFMT) == S_IFDIR) {
		inode_put(inode);
		release_sleep(&vfs_meta_lock);
		inode_put(parent);
		slab_free(name);
		return -1;
	}

	int ret = parent->iops->unlink(parent, name);

	if (ret < 0) {
		inode_put(inode);
		release_sleep(&vfs_meta_lock);
		inode_put(parent);
		slab_free(name);
		return -1;
	}

	// 删除inode缓存引用
	inode_cache_remove(inode);

	// 释放unlink操作持有的引用
	inode_put(inode);

	inode_put(parent);

	release_sleep(&vfs_meta_lock);

	slab_free(name);

	return 0;
}

/// @brief 删除 path 指定的目录。
///
/// @param path 目录路径
///
/// @return 成功0，失败-1
int vfs_rmdir(const char* path)
{
	if (path == NULL)
		return -1;

	struct inode* parent = NULL;

	char* name = slab_alloc(VFS_MAX_PATH_LEN);

	if (name == NULL)
		return -1;

	if (vfs_lookup_parent(path, &parent, name) < 0) {
		slab_free(name);
		return -1;
	}

	if (parent == NULL || parent->iops == NULL ||
	    parent->iops->rmdir == NULL) {
		if (parent)
			inode_put(parent);

		slab_free(name);
		return -1;
	}

	if ((parent->mode & S_IFMT) != S_IFDIR) {
		inode_put(parent);
		slab_free(name);
		return -1;
	}

	struct task_struct* task = get_task();

	if (vfs_permission(parent, &task->cred, MAY_WRITE | MAY_EXEC) < 0) {
		inode_put(parent);
		slab_free(name);
		return -1;
	}

	/*
	 * 并发保护：序列化元数据修改。
	 */
	acquire_sleep(&vfs_meta_lock);

	struct inode* inode = inode_cache_find(parent, name);

	if (inode == NULL) {
		release_sleep(&vfs_meta_lock);
		inode_put(parent);
		slab_free(name);
		return -1;
	}

	// rmdir只能删除目录
	if ((inode->mode & S_IFMT) != S_IFDIR) {
		inode_put(inode);
		release_sleep(&vfs_meta_lock);
		inode_put(parent);
		slab_free(name);
		return -1;
	}

	// 文件系统负责检查目录是否为空并删除目录项
	int ret = parent->iops->rmdir(parent, name);

	if (ret < 0) {
		inode_put(inode);
		release_sleep(&vfs_meta_lock);
		inode_put(parent);
		slab_free(name);
		return -1;
	}

	// 删除inode缓存引用
	inode_cache_remove(inode);

	// 释放rmdir操作持有的引用
	inode_put(inode);

	inode_put(parent);

	release_sleep(&vfs_meta_lock);

	slab_free(name);

	return 0;
}

/*
 * 字符设备文件支持
 * 字符设备（如 /dev/ttyS0）不在任何文件系统里，
 * /dev/ 前缀的路径直接从字符设备表解析，
 * file->inode 为 NULL，file->private 指向 char_device。
 */
static int64_t chardev_file_read(struct file* file, void* buf, uint64_t count)
{
	struct char_device* cdev = file->private;
	uint64_t out_len = 0;

	if (cdev == NULL || cdev->ops == NULL || cdev->ops->read == NULL)
		return -1;

	if (cdev->ops->read(cdev->priv, buf, count, &out_len) < 0)
		return -1;

	return (int64_t)out_len;
}

static int64_t
chardev_file_write(struct file* file, const void* buf, uint64_t count)
{
	struct char_device* cdev = file->private;
	uint64_t out_len = 0;

	if (cdev == NULL || cdev->ops == NULL || cdev->ops->write == NULL)
		return -1;

	if (cdev->ops->write(cdev->priv, buf, count, &out_len) < 0)
		return -1;

	return (int64_t)out_len;
}

static int64_t chardev_file_seek(struct file* file, int64_t offset, int whence)
{
	(void)file;
	(void)offset;
	(void)whence;

	return -1; /* 字符设备不支持 seek */
}

static int chardev_file_close(struct file* file)
{
	struct char_device* cdev = file->private;

	if (cdev != NULL && cdev->ops != NULL && cdev->ops->close != NULL)
		return cdev->ops->close(cdev->priv);

	return 0;
}

static struct file_operations vfs_chardev_fops = {
    .read = chardev_file_read,
    .write = chardev_file_write,
    .seek = chardev_file_seek,
    .close = chardev_file_close,
};

/// @brief 打开一个文件。
///
/// @param path 文件路径
/// @param flags 打开标志
/// @return 成功返回 fd，失败返回 -1
int vfs_open(const char* path, uint32_t flags)
{
	if (path == NULL)
		return -1;

	struct task_struct* task = get_task();

	if (task == NULL)
		return -1;

	/*
	 * /dev/tty 前缀：终端设备
	 */
	if (strncmp(path, "/dev/tty/", 5) == 0) {
		struct char_device* cdev = vfs_find_chardev(path + 5);

		if (cdev == NULL)
			return -1;

		if (cdev->ops != NULL && cdev->ops->open != NULL &&
		    cdev->ops->open(cdev->priv, (int)flags) < 0)
			return -1;

		struct file* file = slab_alloc(sizeof(*file));

		if (file == NULL)
			return -1;

		memset(file, 0, sizeof(*file));

		/* 字符设备没有 inode */
		file->inode = NULL;
		file->pos = 0;
		file->flags = flags;
		file->fops = &vfs_chardev_fops;
		file->private = cdev;

		/*
		 * 初始无持有者
		 * fd_alloc负责增加引用
		 */
		file->refcount = 0;

		init_sleeplock(&file->slk);

		int fd = fd_alloc(file);

		if (fd < 0) {
			slab_free(file);
			return -1;
		}

		return fd;
	}

	struct inode* inode = NULL;

	/*
	 * O_CREAT
	 */
	if (flags & O_CREAT) {
		if (vfs_lookup(path, &inode) < 0) {
			if (vfs_create(path, 0666, &inode) < 0)
				return -1;
		} else if (flags & O_EXCL) {
			inode_put(inode);
			return -1;
		}
	} else {
		if (vfs_lookup(path, &inode) < 0)
			return -1;
	}

	if (inode == NULL)
		return -1;

	/*
	 * O_DIRECTORY
	 */
	if (flags & O_DIRECTORY) {
		if ((inode->mode & S_IFMT) != S_IFDIR) {
			inode_put(inode);
			return -1;
		}
	}

	/*
	 * access mode
	 */
	uint32_t permission = 0;

	switch (flags & O_ACCMODE) {
	case O_RDONLY:
		permission = MAY_READ;
		break;

	case O_WRONLY:
		permission = MAY_WRITE;
		break;

	case O_RDWR:
		permission = MAY_READ | MAY_WRITE;
		break;

	default:
		inode_put(inode);
		return -1;
	}

	if (vfs_permission(inode, &task->cred, permission) < 0) {
		inode_put(inode);
		return -1;
	}

	/*
	 * O_TRUNC
	 */
	if (flags & O_TRUNC) {
		if ((flags & O_ACCMODE) == O_RDONLY) {
			inode_put(inode);
			return -1;
		}

		if (vfs_truncate(inode, 0) < 0) {
			inode_put(inode);
			return -1;
		}
	}

	struct file* file = slab_alloc(sizeof(*file));

	if (file == NULL) {
		inode_put(inode);
		return -1;
	}

	memset(file, 0, sizeof(*file));

	file->inode = inode;
	file->pos = 0;
	file->flags = flags;
	file->fops = inode->fops;

	/*
	 * 初始无持有者
	 * fd_alloc负责增加引用
	 */
	file->refcount = 0;

	init_sleeplock(&file->slk);

	if (flags & O_APPEND)
		file->pos = inode->size;

	int fd = fd_alloc(file);

	if (fd < 0) {
		inode_put(inode);
		slab_free(file);
		return -1;
	}

	return fd;
}

/// @brief 初始化vfs inode缓存表
static void vfs_inode_cache_init()
{
	init_spinlock(&icache.lock);
	icache.table = hash_table_create(
	    VFS_INODE_CACHE_SIZE, inode_cache_hash, inode_cache_key_equal);

	if (icache.table == NULL)
		panic_error("inode_cache_init");
}

/*
 * inode cache 键哈希：
 * 父目录指针与名字一起参与 FNV-1a。
 */
static uint32_t inode_cache_hash(const void* key, uint32_t key_len)
{
	const struct inode_cache_key* k = key;
	const uint8_t* p;
	uint32_t h = 2166136261u;

	(void)key_len;

	p = (const uint8_t*)&k->parent;

	for (uint32_t i = 0; i < sizeof(k->parent); i++) {
		h ^= p[i];
		h *= 16777619u;
	}

	p = (const uint8_t*)k->name;

	for (uint32_t i = 0; i < VFS_MAX_PATH_LEN && p[i] != '\0'; i++) {
		h ^= p[i];
		h *= 16777619u;
	}

	return h;
}

/* inode cache 键比较：父目录指针相同且名字相同 */
static bool
inode_cache_key_equal(const void* key1, const void* key2, uint32_t key_len)
{
	const struct inode_cache_key* a = key1;
	const struct inode_cache_key* b = key2;

	(void)key_len;

	if (a->parent != b->parent)
		return false;

	return strncmp(a->name, b->name, VFS_MAX_PATH_LEN) == 0;
}

void inode_get(struct inode* inode)
{
	if (inode == NULL)
		return;

	acquire(&inode->lock);
	inode->refcount++;
	release(&inode->lock);
}

void inode_put(struct inode* inode)
{
	if (inode == NULL)
		return;

	int destroy = 0;

	acquire(&inode->lock);

	if (inode->refcount == 0) {
		release(&inode->lock);
		return;
	}

	inode->refcount--;

	if (inode->refcount == 0)
		destroy = 1;

	release(&inode->lock);

	if (destroy) {
		if (inode->iops && inode->iops->destroy) {
			inode->iops->destroy(inode);
		} else {
			slab_free(inode);
		}
	}
}

/// @brief 根据父目录 inode 和文件名查找缓存中的 inode
/// @param parent 父目录 inode
/// @param name 文件名
/// @return 命中的 inode，未找到时返回 NULL
struct inode* inode_cache_find(struct inode* parent, const char* name)
{
	if (parent == NULL || name == NULL || name[0] == '\0')
		return NULL;

	struct inode_cache_key* key =
	    slab_alloc(sizeof(struct inode_cache_key));

	if (key == NULL)
		return NULL;

	key->parent = parent;

	if (strlen(name) >= VFS_MAX_PATH_LEN) {
		slab_free(key);
		return NULL;
	}

	strcpy(key->name, name);

	acquire(&icache.lock);

	struct inode* inode =
	    hash_table_lookup(icache.table, key, sizeof(*key));

	if (inode != NULL) {
		acquire(&inode->lock);
		inode->refcount++;
		release(&inode->lock);
	}

	release(&icache.lock);

	slab_free(key);

	return inode;
}

/// @brief 将 inode 插入 inode cache
/// @param inode 要缓存的 inode
/// @return 成功返回 0，inode 已存在或参数非法时返回 -1
int inode_cache_insert(struct inode* inode)
{
	if (inode == NULL || inode->cache_key.parent == NULL ||
	    inode->cache_key.name[0] == '\0') {
		return -1;
	}

	acquire(&icache.lock);

	inode_get(inode);

	bool ret = hash_table_insert_if_absent(
	    icache.table, &inode->cache_key, sizeof(inode->cache_key), inode);

	if (!ret) {
		inode_put(inode);
		release(&icache.lock);
		return -1;
	}

	release(&icache.lock);

	return 0;
}

/// @brief 此函数只会把inode从当前的hashmap中删除且减少映射
///        真正的释放会在inode_put 中处理
/// @param inode 要删除的 inode
void inode_cache_remove(struct inode* inode)
{
	struct inode* parent;
	bool removed = false;

	if (inode == NULL)
		return;

	acquire(&icache.lock);

	removed = hash_table_delete(
	    icache.table, &inode->cache_key, sizeof(inode->cache_key));

	release(&icache.lock);

	if (!removed)
		return;

	/*
	 * 必须在 inode_put 之前读出 parent：
	 * 若 inode 是最后一个引用，put 会触发 destroy，
	 * 之后 cache_key 内存不可再访问。
	 */
	parent = inode->cache_key.parent;

	// 释放cache持有的引用
	inode_put(inode);

	/*
	 * 释放 cache 代表该 inode 持有的父引用。
	 * 根 inode 的 parent 是 SPECIAL 哨兵值，不是真实指针。
	 */
	if ((uint64_t)parent != VFS_ROOT_PARENT_SPECIAL)
		inode_put(parent);
}

/// @brief 截断 inode 对应文件大小
///
/// VFS 层职责（开发范式）：
///   1. 权限校验——调用者必须对 inode 具有 MAY_WRITE
///   2. 并发保护——通过 vfs_meta_lock 序列化元数据修改
///
/// @param inode 目标 inode
/// @param size 新文件大小
/// @return 成功返回0，失败返回-1
int vfs_truncate(struct inode* inode, uint64_t size)
{
	if (inode == NULL)
		return -1;

	struct task_struct* task = get_task();

	if (vfs_permission(inode, &task->cred, MAY_WRITE) < 0) {
		return -1;
	}

	if (inode->iops == NULL || inode->iops->truncate == NULL) {
		return -1;
	}

	acquire_sleep(&vfs_meta_lock);

	int ret = inode->iops->truncate(inode, size);

	release_sleep(&vfs_meta_lock);

	return ret;
}

void file_get(struct file* file)
{
	if (file == NULL)
		return;

	acquire_sleep(&file->slk);

	file->refcount++;

	release_sleep(&file->slk);
}

void file_put(struct file* file)
{
	if (file == NULL)
		return;

	int destroy = 0;

	acquire_sleep(&file->slk);

	if (file->refcount > 0)
		file->refcount--;

	if (file->refcount == 0)
		destroy = 1;

	release_sleep(&file->slk);

	if (!destroy)
		return;

	if (file->fops && file->fops->close) {
		file->fops->close(file);
	}

	if (file->inode)
		inode_put(file->inode);

	slab_free(file);
}

int64_t vfs_read(struct file* file, void* buf, uint64_t count)
{
	if (file == NULL || buf == NULL)
		return -1;

	if (file->fops == NULL || file->fops->read == NULL)
		return -1;

	acquire_sleep(&file->slk);

	int64_t ret = file->fops->read(file, buf, count);
	if (ret > 0)
		file->pos += ret;
	release_sleep(&file->slk);

	return ret;
}

int64_t vfs_write(struct file* file, const void* buf, uint64_t count)
{
	if (file == NULL || buf == NULL)
		return -1;

	if (file->fops == NULL || file->fops->write == NULL)
		return -1;

	acquire_sleep(&file->slk);

	int64_t ret = file->fops->write(file, buf, count);

	if (ret > 0)
		file->pos += ret;

	release_sleep(&file->slk);

	return ret;
}

void vfs_close(struct file* file)
{
	file_put(file);
}

int fd_alloc(struct file* file)
{
	if (file == NULL)
		return -1;

	struct task_struct* task = get_task();

	if (task == NULL)
		return -1;

	acquire(&task->lk);

	for (int i = 0; i < NOFILE; i++) {
		if (task->ofile[i] == NULL) {
			task->ofile[i] = file;

			file_get(file);

			release(&task->lk);
			return i;
		}
	}

	release(&task->lk);

	return -1;
}

struct file* fd_get(int fd)
{
	struct task_struct* task = get_task();

	if (task == NULL)
		return NULL;

	if (fd < 0 || fd >= NOFILE)
		return NULL;

	acquire(&task->lk);

	struct file* file = task->ofile[fd];

	if (file != NULL) {
		file_get(file);
	}

	release(&task->lk);

	return file;
}

/// @brief 复制一个文件描述符。
/// @param oldfd 原文件描述符
/// @return 成功返回新的 fd，失败返回 -1
int vfs_dup(int oldfd)
{
	struct file* file = fd_get(oldfd);

	if (file == NULL)
		return -1;

	int newfd = fd_alloc(file);

	/*
	 * 释放 fd_get 获取的临时引用
	 */
	file_put(file);

	return newfd;
}

int fd_close(int fd)
{
	struct task_struct* task = get_task();

	if (task == NULL)
		return -1;

	if (fd < 0 || fd >= NOFILE)
		return -1;

	struct file* file = NULL;

	acquire(&task->lk);

	file = task->ofile[fd];

	if (file == NULL) {
		release(&task->lk);
		return -1;
	}

	/*
	 * 解除 fd 对 file 的引用
	 */
	task->ofile[fd] = NULL;
	release(&task->lk);

	/*
	 * 释放 fd 持有的 file 引用
	 */
	vfs_close(file);
	return 0;
}

int64_t vfs_seek(struct file* file, int64_t offset, int whence)
{
	if (file == NULL)
		return -1;

	if (file->fops == NULL || file->fops->seek == NULL)
		return -1;

	acquire_sleep(&file->slk);

	int64_t ret = file->fops->seek(file, offset, whence);
	/*
	 * seek 成功后，
	 * filesystem 返回新的位置。
	 *
	 * VFS 更新 file->pos。
	 */
	if (ret >= 0)
		file->pos = ret;

	release_sleep(&file->slk);

	return ret;
}

/// @brief 获取打开文件的属性。
///
/// @param file 打开的文件对象
/// @param stat 输出文件属性
///
/// @return 成功返回 0，失败返回 -1
int vfs_fstat(struct file* file, struct vfs_kstat* stat)
{
	if (file == NULL || stat == NULL)
		return -1;

	struct inode* inode = file->inode;

	if (inode == NULL)
		return -1;

	if (inode->iops == NULL || inode->iops->getattr == NULL)
		return -1;

	return inode->iops->getattr(inode, stat);
}

/// @brief 读取目录中的下一项。
///
/// @param file 打开的目录文件
/// @param dirent 输出目录项
///
/// @return 成功返回 0，结束或失败返回 -1
int vfs_readdir(struct file* file, struct vfs_dirent* dirent)
{
	if (file == NULL || dirent == NULL)
		return -1;

	struct inode* inode = file->inode;

	if (inode == NULL)
		return -1;

	/*
	 * 只有目录才能 readdir
	 */
	if ((inode->mode & S_IFMT) != S_IFDIR)
		return -1;

	if (inode->iops == NULL || inode->iops->readdir == NULL)
		return -1;

	acquire_sleep(&file->slk);

	int ret = inode->iops->readdir(inode, &file->pos, dirent);

	release_sleep(&file->slk);

	return ret;
}

int64_t vfs_seek_fd(int fd, int64_t offset, int whence)
{
	struct file* file = fd_get(fd);

	if (file == NULL)
		return -1;

	int64_t ret = vfs_seek(file, offset, whence);

	file_put(file);

	return ret;
}

/// @brief 为当前进程建立标准输入/输出/错误（fd 0/1/2）。
///
/// 三个 fd 都指向控制台字符设备 /dev/ttyS0。
/// 必须在 ofile[] 全空的进程上调用，fd 依次占用 0、1、2。
void init_vfs_std(void)
{
	for (int i = 0; i < 3; i++) {
		if (vfs_open(VFS_STD_DEVICE, O_RDWR) < 0)
			panic_error("init_vfs_std: open %s failed",
				    VFS_STD_DEVICE);
	}
}

/*
 * umount
 */

/// @brief 在挂载树中按路径精确查找挂载点。
/// 与 vfs_find_mount 的"最深覆盖"语义不同，这里要求
/// mount->path 与 target 完全一致。
/// 调用者必须持有 mount_table.lock。
static struct mount* vfs_find_mount_exact(struct mount* node, const char* path)
{
	struct mount* c;

	if (node == NULL)
		return NULL;

	c = node->child;

	while (c != NULL) {
		struct mount* r;

		if (strcmp(c->path, path) == 0)
			return c;

		r = vfs_find_mount_exact(c, path);

		if (r != NULL)
			return r;

		c = c->next;
	}

	return NULL;
}

struct vfs_umount_collect_arg {
	struct inode** victims;
	uint32_t max;
	uint32_t count;
	struct super_block* sb;
};

/* 在 hash_table_foreach 持锁回调里收集该 sb 的缓存 inode */
static void
vfs_umount_collect_cb(const void* key, uint32_t key_len, void* value, void* arg)
{
	struct vfs_umount_collect_arg* a = arg;
	struct inode* ino = value;

	(void)key;
	(void)key_len;

	if (a->count < a->max && ino != NULL && ino->sb == a->sb)
		a->victims[a->count++] = ino;
}

struct mount* vfs_get_root_mount(void)
{
	return mount_table.root;
}

/// @brief 卸载挂载点 target。
///
/// 步骤：
///   1. 拒绝卸载根文件系统 "/";
///   2. 按路径精确找到挂载点，有子挂载点则拒绝；
///   3. 扫描全部进程，任一 ofile 或 cwd 引用该 sb 则拒绝（busy）；
///   4. 从父挂载点的孩子链表摘除；
///   5. 清除该 sb 在 inode cache 的所有残留条目
///      （每个条目持有 inode 自身与其父目录的引用）；
///   6. kill_sb 释放 sb 持有的 root 引用并销毁 sb，最后释放 mount。
///
/// @param target 挂载点路径，例如 "/mnt"
/// @return 成功返回 0，失败返回 -1
int vfs_umount(const char* target)
{
	struct mount* mnt = NULL;
	struct mount* parent = NULL;
	struct super_block* sb;
	struct filesystem* fs;
	struct inode** victims;
	struct vfs_umount_collect_arg carg;
	uint32_t retry;

	if (target == NULL || target[0] != '/' || strcmp(target, "/") == 0)
		return -1;

	acquire(&mount_table.lock);

	mnt = vfs_find_mount_exact(mount_table.root, target);

	if (mnt == NULL) {
		release(&mount_table.lock);
		return -1;
	}

	/* 还有子挂载点时不能卸载 */
	if (mnt->child != NULL) {
		release(&mount_table.lock);
		return -1;
	}

	sb = mnt->sb;

	if (sb == NULL || sb->fs == NULL) {
		release(&mount_table.lock);
		return -1;
	}

	/*
	 * busy 检查：任何进程的 ofile / cwd 引用该 sb 都拒绝卸载。
	 * 字符设备文件 inode 为 NULL，天然跳过。
	 */
	for (uint32_t i = 0; i < NTASKS; i++) {
		struct task_struct* t = &tasks[i];

		if (t->cwd != NULL && t->cwd->sb == sb) {
			release(&mount_table.lock);
			return -1;
		}

		for (int j = 0; j < NOFILE; j++) {
			struct file* f = t->ofile[j];

			if (f != NULL && f->inode != NULL &&
			    f->inode->sb == sb) {
				release(&mount_table.lock);
				return -1;
			}
		}
	}

	/*
	 * 从父挂载点的孩子链表中摘除
	 */
	parent = mnt->parent;

	if (parent != NULL) {
		struct mount** pp = &parent->child;

		while (*pp != NULL && *pp != mnt)
			pp = &(*pp)->next;

		if (*pp == mnt)
			*pp = mnt->next;
	}

	fs = sb->fs;

	release(&mount_table.lock);

	/*
	 * 清除该 sb 在 inode cache 中的所有残留。
	 * 不能在 foreach 回调里直接删除，先收集再逐个 remove。
	 * remove 会级联释放父子引用，一轮删不完就再来一轮。
	 */
	victims = slab_alloc(sizeof(struct inode*) * VFS_INODE_CACHE_SIZE);

	if (victims == NULL) {
		/* 摘链后分配失败：重新挂回去，保持挂载表一致 */
		acquire(&mount_table.lock);

		if (parent != NULL) {
			mnt->next = parent->child;
			parent->child = mnt;
		}

		release(&mount_table.lock);
		return -1;
	}

	for (retry = 0; retry < 8; retry++) {
		carg.victims = victims;
		carg.max = VFS_INODE_CACHE_SIZE;
		carg.count = 0;
		carg.sb = sb;

		hash_table_foreach(icache.table, vfs_umount_collect_cb, &carg);

		if (carg.count == 0)
			break;

		for (uint32_t i = 0; i < carg.count; i++)
			inode_cache_remove(victims[i]);
	}

	slab_free(victims);

	/*
	 * root 的 cache 残留（含挂载点父引用）已在上面的循环中清除，
	 * 这里只需释放 sb 持有的 root 引用并销毁 sb。
	 */
	fs->kill_sb(sb);

	slab_free(mnt);

	return 0;
}