#include <ext2.h>
#include <memory.h>
#include <block_device.h>
#include <printk.h>
#include <vfs.h>
#include <lib.h>
#include <timer.h>
#include <slab.h>
#include <spinlock.h>
#include <hashmap.h>

/*
 * ext2 文件系统实现
 *
 * 设计目标：
 *   - 最精简的 ext2 rev 0/1 实现，支持读写
 *   - 块大小 1024 / 2048 / 4096
 *   - 12 direct + 1 indirect + 1 double indirect
 *   - 线性目录项（变长 linked-list）
 *   - 不支持 symlink、设备文件、扩展属性、ACL、journaling
 *   - 第一空闲槽位分配（bitmap）
 *
 * 引用约定
 *   - lookup/create/mkdir 返回的 inode refcount=1，引用归 caller
 *   - sb->root 的 refcount=1，引用归 super_block
 *   - destroy 负责 free private 与 inode 本身
 *
 * 限制：
 *   - 三级间接块（i_block[14]）不被支持，超过 ~64MB 的文件读写会失败
 *   - 不合并删除目录项产生的"洞"，下次 add 复用空闲槽位（inode==0）
 */

/*
 * 磁盘结构
 */

struct ext2_superblock {
	uint32_t s_inodes_count;
	uint32_t s_blocks_count;
	uint32_t s_r_blocks_count;
	uint32_t s_free_blocks_count;
	uint32_t s_free_inodes_count;
	uint32_t s_first_data_block;
	uint32_t s_log_block_size;
	uint32_t s_log_frag_size;
	uint32_t s_blocks_per_group;
	uint32_t s_frags_per_group;
	uint32_t s_inodes_per_group;
	uint32_t s_mtime;
	uint32_t s_wtime;
	uint16_t s_mnt_count;
	uint16_t s_max_mnt_count;
	uint16_t s_magic;
	uint16_t s_state;
	uint16_t s_errors;
	uint16_t s_minor_rev_level;
	uint32_t s_lastcheck;
	uint32_t s_checkinterval;
	uint32_t s_creator_os;
	uint32_t s_rev_level;
	uint16_t s_def_resuid;
	uint16_t s_def_resgid;
	/* ---- 仅 rev 1 (s_rev_level != 0) 有效，rev 0 读到 0 ---- */
	uint32_t s_first_ino; /* 偏移 84：第一个非保留 inode 号 */
	uint16_t
	    s_inode_size; /* 偏移 88：每个磁盘 inode 的大小（128/256...） */
	uint16_t s_block_group_nr; /* 偏移 90：备份超级块所属组号 */
	uint32_t s_feature_compat; /* 偏移 92 */
	uint32_t s_feature_incompat;
	uint32_t s_feature_ro_compat;
	/* 其余 rev1 字段（uuid/volume name/...）不解析，保留占位即可 */
	uint32_t s_rev1_rest[36];
} __attribute__((packed));

struct ext2_bg_desc {
	uint32_t bg_block_bitmap;
	uint32_t bg_inode_bitmap;
	uint32_t bg_inode_table;
	uint16_t bg_free_blocks_count;
	uint16_t bg_free_inodes_count;
	uint16_t bg_used_dirs_count;
	uint16_t bg_pad;
	uint32_t bg_reserved[3];
} __attribute__((packed));

struct ext2_inode {
	uint16_t i_mode;	/* 偏移 0  */
	uint16_t i_uid;		/* 偏移 2  */
	uint32_t i_size;	/* 偏移 4  */
	uint32_t i_atime;	/* 偏移 8  */
	uint32_t i_ctime;	/* 偏移 12 */
	uint32_t i_mtime;	/* 偏移 16 */
	uint32_t i_dtime;	/* 偏移 20 */
	uint16_t i_gid;		/* 偏移 24 */
	uint16_t i_links_count; /* 偏移 26 */
	/*
	 * 注意：i_blocks 在偏移 28，不能漏！
	 * 它是文件占用的 512 字节扇区数（不是逻辑块数）。
	 * 漏掉会导致 i_flags / i_osd1 / i_block[] 全部左移 4 字节，
	 * i_block[0] 误读成保留字（恒 0），所有数据块访问错位。
	 */
	uint32_t i_blocks;		 /* 偏移 28 */
	uint32_t i_flags;		 /* 偏移 32 */
	uint32_t i_osd1;		 /* 偏移 36 */
	uint32_t i_block[EXT2_N_BLOCKS]; /* 偏移 40 */
	uint32_t i_generation;		 /* 偏移 100 */
	uint32_t i_file_acl;		 /* 偏移 104 */
	uint32_t i_dir_acl;		 /* 偏移 108 */
	uint32_t i_faddr;		 /* 偏移 112 */
	uint8_t i_osd2[12];		 /* 偏移 116..127 */
} __attribute__((packed));
/* 标准磁盘 inode 基础部分必须为 128 字节；错位会在编译期立即暴露 */
_Static_assert(sizeof(struct ext2_inode) == 128,
	       "ext2 inode base size must be 128 bytes");

struct ext2_dirent {
	uint32_t inode;
	uint16_t rec_len;
	uint8_t name_len;
	uint8_t file_type;
	char name[];
} __attribute__((packed));

/*
 * 前向声明
 *  */

static struct inode_operations ext2_inode_ops;
static struct file_operations ext2_file_ops;

static void ext2_inode_destroy(struct inode* inode);
static int ext2_truncate(struct inode* inode, uint64_t size);

/*
 * 工具：块 I/O
 *  */

static inline uint32_t ext2_block_sectors(struct ext2_fs_priv* fs)
{
	return fs->block_size / 512;
}

static inline uint64_t ext2_block_to_sector(struct ext2_fs_priv* fs,
					    uint32_t block)
{
	return (uint64_t)block * ext2_block_sectors(fs);
}

/*
 * 块缓存（write-back buffer cache）
 *
 * mkdir / create / unlink 等单次操作会反复读写同一批元数据块
 *（inode bitmap、block bitmap、inode 表块、父目录块）。
 * 无缓存时每次 ext2_read_block / ext2_write_block 都同步走 virtio，
 * 一个 4K 块要 8 次 512B 扇区往返，叠加起来就是 mkdir 的明显卡顿。
 *
 * 这里在 fs_priv 上挂一张 hashmap：键 = 块号(uint32_t)，值 = cache_entry。
 *   - read_block：命中则 memcpy 返回，未命中走下面的 raw 读入缓存再 memcpy。
 *   - write_block：只写缓存并标记 dirty，不立即落盘。
 *   - ext2_cache_flush：在顶层修改操作返回前一次性把所有 dirty 块写回磁盘。
 * 只读路径（lookup / readdir）不写盘，纯读缓存，命中即返回。
 *
 * 注意：cache 内部（miss 读盘、flush 写盘）必须调 raw 版本，
 * 否则 ext2_write_block → cache_lookup → hash_table_lookup 会与
 * flush 回调里持有的 ht->lock 递归死锁。
 */
struct ext2_cache_entry {
	uint32_t block; /* 键：块号；&entry->block 作为 hashmap 的 key */
	uint8_t* data;	/* block_size 字节的块数据 */
	bool dirty;	/* 是否被修改过、尚未写回 */
};

static int
ext2_disk_read_raw(struct ext2_fs_priv* fs, uint32_t block, void* buf)
{
	uint64_t sector = ext2_block_to_sector(fs, block);
	uint32_t nsec = ext2_block_sectors(fs);

	for (uint32_t i = 0; i < nsec; i++) {
		if (fs->bdev->driver.read(fs->bdev->private_data,
					  sector + i,
					  (uint8_t*)buf + i * 512) < 0)
			return -1;
	}

	return 0;
}

static int
ext2_disk_write_raw(struct ext2_fs_priv* fs, uint32_t block, const void* buf)
{
	uint64_t sector = ext2_block_to_sector(fs, block);
	uint32_t nsec = ext2_block_sectors(fs);

	for (uint32_t i = 0; i < nsec; i++) {
		if (fs->bdev->driver.write(fs->bdev->private_data,
					   sector + i,
					   (const uint8_t*)buf + i * 512) < 0)
			return -1;
	}

	return 0;
}

static int ext2_cache_init(struct ext2_fs_priv* fs)
{
	fs->cache = hash_table_create(16, NULL, NULL);
	return fs->cache == NULL ? -1 : 0;
}

/*
 * flush 收集上下文。
 * hash_table_foreach 的回调全程持有 ht->lock（自旋锁，持锁期间中断关闭），
 * 而 virtio 同步 IO 在轮询前会 intr_on() 重新开中断，轮询期间可能被
 * 时钟中断抢占并调度——持自旋锁进入 sched() 会让 noff==2，触发
 * "sched: noff != 1" panic。
 *
 * 因此回调内【绝不能做 IO】，只能把 dirty 条目指针收集到数组并清 dirty；
 * 真正的 raw 写盘在 foreach 返回（锁已释放）之后进行。
 */
struct ext2_flush_ctx {
	struct ext2_cache_entry** ents;
	uint32_t cap;
	uint32_t n;
};

/* foreach 回调：只收集 dirty 条目指针并清 dirty，不做 IO。 */
static void
ext2_cache_gather_cb(const void* key, uint32_t key_len, void* value, void* arg)
{
	struct ext2_flush_ctx* ctx = arg;
	struct ext2_cache_entry* e = value;

	(void)key;
	(void)key_len;

	if (e != NULL && e->dirty && ctx->n < ctx->cap) {
		/* 先清 dirty：写成功就保持清除；写失败再重新置位 */
		e->dirty = false;
		ctx->ents[ctx->n++] = e;
	}
}

/*
 * 把所有 dirty 块写回磁盘。
 * 必须在 vfs_meta_lock 保护下、或 kill_sb 中调用；
 * 调用期间不得持有其它自旋锁（raw 写盘会开中断并可能调度）。
 * 返回 0 成功；有块写失败返回 -1（失败块已重新置 dirty，下次重试）。
 */
#define EXT2_FLUSH_BATCH 512 /* 512 * 8B = 4096B，恰为 slab 单次上限 */

static int ext2_cache_flush(struct ext2_fs_priv* fs)
{
	struct ext2_flush_ctx ctx;
	int ret = 0;

	if (fs == NULL || fs->cache == NULL)
		return 0;

	ctx.ents = slab_alloc(EXT2_FLUSH_BATCH * sizeof(void*));
	if (ctx.ents == NULL)
		return -1;

	/*
	 * 分批收集 + 锁外写盘：
	 * 每批最多 EXT2_FLUSH_BATCH 个；一批全部写失败则终止，
	 * 避免设备持续故障时死循环。
	 */
	for (;;) {
		uint32_t failed = 0;

		ctx.cap = EXT2_FLUSH_BATCH;
		ctx.n = 0;
		hash_table_foreach(fs->cache, ext2_cache_gather_cb, &ctx);

		if (ctx.n == 0)
			break;

		/* foreach 已返回，ht->lock 已释放，此处做 raw 写盘 */
		for (uint32_t i = 0; i < ctx.n; i++) {
			struct ext2_cache_entry* e = ctx.ents[i];
			if (ext2_disk_write_raw(fs, e->block, e->data) < 0) {
				e->dirty = true; /* 下次 flush 重试 */
				failed++;
				ret = -1;
			}
		}

		/* 本批没装满，说明缓存中已无遗留 dirty 项 */
		if (ctx.n < ctx.cap)
			break;
		/* 整批全失败：设备异常，继续重试无意义 */
		if (failed == ctx.n)
			break;
	}

	slab_free(ctx.ents);
	return ret;
}

/* foreach 回调：释放每个条目的 data 与条目本身。 */
static void
ext2_cache_free_cb(const void* key, uint32_t key_len, void* value, void* arg)
{
	(void)key;
	(void)key_len;
	(void)arg;
	if (value != NULL) {
		struct ext2_cache_entry* e = value;
		if (e->data != NULL)
			slab_free(e->data);
		slab_free(e);
	}
}

static void ext2_cache_destroy(struct ext2_fs_priv* fs)
{
	if (fs == NULL || fs->cache == NULL)
		return;
	hash_table_foreach(fs->cache, ext2_cache_free_cb, NULL);
	hash_table_destroy(fs->cache);
	fs->cache = NULL;
}

/*
 * 走缓存的块读：命中直接 memcpy；未命中读盘并缓存。
 * buf 由调用者提供，至少 block_size 字节。
 */
static int ext2_read_block(struct ext2_fs_priv* fs, uint32_t block, void* buf)
{
	uint32_t key = block;
	struct ext2_cache_entry* e;

	if (fs->cache != NULL) {
		e = hash_table_lookup(fs->cache, &key, sizeof(key));
		if (e != NULL) {
			memcpy(buf, e->data, fs->block_size);
			return 0;
		}
	}

	/* 未命中：从磁盘读入缓存，再 memcpy 给调用者 */
	if (fs->cache != NULL) {
		e = slab_alloc(sizeof(*e));
		if (e != NULL) {
			e->block = block;
			e->data = slab_alloc(fs->block_size);
			e->dirty = false;
			if (e->data == NULL) {
				slab_free(e);
			} else if (ext2_disk_read_raw(fs, block, e->data) < 0) {
				slab_free(e->data);
				slab_free(e);
			} else {
				/* 若已存在（并发），覆盖旧值会泄漏，先 delete
				 */
				uint32_t k2 = block;
				if (hash_table_lookup(
					fs->cache, &k2, sizeof(k2)) != NULL)
					hash_table_delete(
					    fs->cache, &k2, sizeof(k2));
				if (!hash_table_insert_if_absent(
					fs->cache,
					&e->block,
					sizeof(e->block),
					e)) {
					/* 竞态失败：直接放掉，落回 raw 路径 */
					slab_free(e->data);
					slab_free(e);
				} else {
					memcpy(buf, e->data, fs->block_size);
					return 0;
				}
			}
		}
	}

	/* 缓存不可用或分配失败：退化到直接读盘 */
	return ext2_disk_read_raw(fs, block, buf);
}

/*
 * 走缓存的块写：只写缓存标 dirty，不立即落盘。
 * 由 ext2_cache_flush() 在顶层操作末尾统一写回。
 */
static int
ext2_write_block(struct ext2_fs_priv* fs, uint32_t block, const void* buf)
{
	uint32_t key = block;
	struct ext2_cache_entry* e;

	if (fs->cache != NULL) {
		e = hash_table_lookup(fs->cache, &key, sizeof(key));
		if (e != NULL) {
			memcpy(e->data, buf, fs->block_size);
			e->dirty = true;
			return 0;
		}

		/* 不在缓存：分配新条目插入 */
		e = slab_alloc(sizeof(*e));
		if (e != NULL) {
			e->block = block;
			e->data = slab_alloc(fs->block_size);
			e->dirty = true;
			if (e->data == NULL) {
				slab_free(e);
			} else {
				memcpy(e->data, buf, fs->block_size);
				if (!hash_table_insert_if_absent(
					fs->cache,
					&e->block,
					sizeof(e->block),
					e)) {
					/* 已存在：查到旧条目更新 */
					uint32_t k2 = block;
					struct ext2_cache_entry* old =
					    hash_table_lookup(
						fs->cache, &k2, sizeof(k2));
					if (old != NULL) {
						memcpy(old->data,
						       buf,
						       fs->block_size);
						old->dirty = true;
					}
					slab_free(e->data);
					slab_free(e);
					return 0;
				}
				return 0;
			}
		}
	}

	/* 缓存不可用：退化到直接写盘 */
	return ext2_disk_write_raw(fs, block, buf);
}

/* 读超级块所在的扇区，结果放在 1024 字节缓冲区 */
static int ext2_read_super(struct block_device* bdev, void* buf)
{
	/* 超级块固定在字节 1024 处，跨 2 个 512 扇区 */
	if (bdev->driver.read(bdev->private_data, 2, buf) < 0)
		return -1;

	if (bdev->driver.read(bdev->private_data, 3, (uint8_t*)buf + 512) < 0)
		return -1;

	return 0;
}

/*
 * 时间戳
 *  */

static uint32_t ext2_now(void)
{
	uint64_t ticks = get_sys_timer_tick();
	return (uint32_t)(ticks / 100) + 1700000000U;
}

/*
 * 超级块 / 块组描述符 读写
 *  */

/* 超级块固定在字节偏移 1024，对应块号：
 *   block_size = 1024 → block 1
 *   block_size > 1024 → block 0（块 0 包含 boot sector 与 superblock）
 */
static uint32_t ext2_sb_block(struct ext2_fs_priv* fs)
{
	return fs->first_data_block == 0 ? 0 : 1;
}

static int ext2_load_super(struct ext2_fs_priv* fs, struct ext2_superblock* sb)
{
	uint8_t* buf = slab_alloc(fs->block_size);

	if (buf == NULL)
		return -1;

	if (ext2_read_block(fs, ext2_sb_block(fs), buf) < 0) {
		slab_free(buf);
		return -1;
	}

	memcpy(sb,
	       buf + (fs->block_size == 1024 ? 0 : 1024 % fs->block_size),
	       sizeof(*sb));

	slab_free(buf);
	return 0;
}

static int ext2_save_super(struct ext2_fs_priv* fs,
			   const struct ext2_superblock* sb)
{
	uint8_t* buf = slab_alloc(fs->block_size);
	uint32_t sb_off;
	int ret;

	if (buf == NULL)
		return -1;

	sb_off = (fs->block_size == 1024) ? 0 : (1024 % fs->block_size);

	/* 读回整块，仅覆盖 superblock 区域，保留 boot sector */
	if (ext2_read_block(fs, ext2_sb_block(fs), buf) < 0) {
		slab_free(buf);
		return -1;
	}

	memcpy(buf + sb_off, sb, sizeof(*sb));

	ret = ext2_write_block(fs, ext2_sb_block(fs), buf);

	slab_free(buf);
	return ret;
}

/* 块组描述符表紧跟超级块所在块的下一块 */
static uint32_t ext2_bgdt_block(struct ext2_fs_priv* fs)
{
	return ext2_sb_block(fs) + 1;
}

static int
ext2_load_bg(struct ext2_fs_priv* fs, uint32_t group, struct ext2_bg_desc* out)
{
	uint32_t bgs_per_block = fs->block_size / sizeof(struct ext2_bg_desc);
	uint32_t blk = ext2_bgdt_block(fs) + group / bgs_per_block;
	uint32_t idx = group % bgs_per_block;
	uint8_t* buf;
	int ret;

	buf = slab_alloc(fs->block_size);
	if (buf == NULL)
		return -1;

	if (ext2_read_block(fs, blk, buf) < 0) {
		slab_free(buf);
		return -1;
	}

	memcpy(out, buf + idx * sizeof(*out), sizeof(*out));

	slab_free(buf);
	return 0;
}

static int ext2_save_bg(struct ext2_fs_priv* fs,
			uint32_t group,
			const struct ext2_bg_desc* bg)
{
	uint32_t bgs_per_block = fs->block_size / sizeof(struct ext2_bg_desc);
	uint32_t blk = ext2_bgdt_block(fs) + group / bgs_per_block;
	uint32_t idx = group % bgs_per_block;
	uint8_t* buf;
	int ret;

	buf = slab_alloc(fs->block_size);
	if (buf == NULL)
		return -1;

	if (ext2_read_block(fs, blk, buf) < 0) {
		slab_free(buf);
		return -1;
	}

	memcpy(buf + idx * sizeof(*bg), bg, sizeof(*bg));

	ret = ext2_write_block(fs, blk, buf);

	slab_free(buf);
	return ret;
}

/*
 * Bitmap 操作
 *  */

static int
ext2_bitmap_test(struct ext2_fs_priv* fs, uint32_t bitmap_block, uint32_t bit)
{
	uint8_t* buf;
	uint8_t mask;
	int ret;

	buf = slab_alloc(fs->block_size);
	if (buf == NULL)
		return -1;

	if (ext2_read_block(fs, bitmap_block, buf) < 0) {
		slab_free(buf);
		return -1;
	}

	mask = 1u << (bit % 8);
	ret = (buf[bit / 8] & mask) ? 1 : 0;

	slab_free(buf);
	return ret;
}

static int
ext2_bitmap_set(struct ext2_fs_priv* fs, uint32_t bitmap_block, uint32_t bit)
{
	uint8_t* buf;
	uint8_t mask;
	int ret;

	buf = slab_alloc(fs->block_size);
	if (buf == NULL)
		return -1;

	if (ext2_read_block(fs, bitmap_block, buf) < 0) {
		slab_free(buf);
		return -1;
	}

	mask = 1u << (bit % 8);
	buf[bit / 8] |= mask;

	ret = ext2_write_block(fs, bitmap_block, buf);

	slab_free(buf);
	return ret;
}

static int
ext2_bitmap_clear(struct ext2_fs_priv* fs, uint32_t bitmap_block, uint32_t bit)
{
	uint8_t* buf;
	uint8_t mask;
	int ret;

	buf = slab_alloc(fs->block_size);
	if (buf == NULL)
		return -1;

	if (ext2_read_block(fs, bitmap_block, buf) < 0) {
		slab_free(buf);
		return -1;
	}

	mask = 1u << (bit % 8);
	buf[bit / 8] &= ~mask;

	ret = ext2_write_block(fs, bitmap_block, buf);

	slab_free(buf);
	return ret;
}

/*
 * 磁盘 inode 读写
 *  */

static int
ext2_inode_read(struct ext2_fs_priv* fs, uint32_t ino, struct ext2_inode* out)
{
	uint32_t group = (ino - 1) / fs->inodes_per_group;
	uint32_t idx = (ino - 1) % fs->inodes_per_group;
	struct ext2_bg_desc bg;
	uint32_t inode_block;
	uint32_t inode_off;
	uint8_t* buf;
	int ret;

	if (ino == 0 || ino > fs->inodes_count)
		return -1;

	if (ext2_load_bg(fs, group, &bg) < 0)
		return -1;

	inode_block =
	    bg.bg_inode_table + (idx * fs->inode_size) / fs->block_size;
	inode_off = (idx * fs->inode_size) % fs->block_size;

	buf = slab_alloc(fs->block_size);
	if (buf == NULL)
		return -1;

	if (ext2_read_block(fs, inode_block, buf) < 0) {
		slab_free(buf);
		return -1;
	}

	memcpy(out, buf + inode_off, sizeof(*out));

	slab_free(buf);
	return 0;
}

static int ext2_inode_write(struct ext2_fs_priv* fs,
			    uint32_t ino,
			    const struct ext2_inode* inode)
{
	uint32_t group = (ino - 1) / fs->inodes_per_group;
	uint32_t idx = (ino - 1) % fs->inodes_per_group;
	struct ext2_bg_desc bg;
	uint32_t inode_block;
	uint32_t inode_off;
	uint8_t* buf;
	int ret;

	if (ino == 0 || ino > fs->inodes_count)
		return -1;

	if (ext2_load_bg(fs, group, &bg) < 0)
		return -1;

	inode_block =
	    bg.bg_inode_table + (idx * fs->inode_size) / fs->block_size;
	inode_off = (idx * fs->inode_size) % fs->block_size;

	buf = slab_alloc(fs->block_size);
	if (buf == NULL)
		return -1;

	if (ext2_read_block(fs, inode_block, buf) < 0) {
		slab_free(buf);
		return -1;
	}

	memcpy(buf + inode_off, inode, sizeof(*inode));

	ret = ext2_write_block(fs, inode_block, buf);

	slab_free(buf);
	return ret;
}

/*
 * Inode / Block 分配
 *  */

/* 分配一个空闲 inode，返回 inode number（1-based）。
 * 简单策略：从 group 0 开始线性扫描 inode bitmap。 */
static int ext2_alloc_inode(struct ext2_fs_priv* fs, uint32_t* out)
{
	for (uint32_t g = 0; g < fs->num_groups; g++) {
		struct ext2_bg_desc bg;
		uint32_t ino_base = g * fs->inodes_per_group + 1;

		if (ext2_load_bg(fs, g, &bg) < 0)
			return -1;

		for (uint32_t i = 0; i < fs->inodes_per_group; i++) {
			int used = ext2_bitmap_test(fs, bg.bg_inode_bitmap, i);
			if (used < 0)
				return -1;
			if (used)
				continue;

			if (ext2_bitmap_set(fs, bg.bg_inode_bitmap, i) < 0)
				return -1;

			bg.bg_free_inodes_count--;
			if (ext2_save_bg(fs, g, &bg) < 0)
				return -1;

			fs->free_inodes--;
			*out = ino_base + i;
			return 0;
		}
	}

	return -1;
}

static int ext2_free_inode(struct ext2_fs_priv* fs, uint32_t ino)
{
	uint32_t group = (ino - 1) / fs->inodes_per_group;
	uint32_t idx = (ino - 1) % fs->inodes_per_group;
	struct ext2_bg_desc bg;

	if (ext2_load_bg(fs, group, &bg) < 0)
		return -1;

	if (ext2_bitmap_clear(fs, bg.bg_inode_bitmap, idx) < 0)
		return -1;

	bg.bg_free_inodes_count++;
	if (ext2_save_bg(fs, group, &bg) < 0)
		return -1;

	fs->free_inodes++;
	return 0;
}

/* 分配一个空闲数据块，返回块号。
 * 简单策略：从 group 0 开始线性扫描 block bitmap。 */
static int ext2_alloc_block(struct ext2_fs_priv* fs, uint32_t* out)
{
	for (uint32_t g = 0; g < fs->num_groups; g++) {
		struct ext2_bg_desc bg;
		uint32_t blk_base =
		    fs->first_data_block + g * fs->blocks_per_group;

		if (ext2_load_bg(fs, g, &bg) < 0)
			return -1;

		/* 块位图的 bit i 对应物理块 blk_base + i。
		 * bit 0 对应 blk_base，bit 1 对应 blk_base+1，依此类推。 */
		for (uint32_t i = 0; i < fs->blocks_per_group; i++) {
			int used;
			uint32_t blk = blk_base + i;

			if (blk >= fs->blocks_count)
				break;

			used = ext2_bitmap_test(fs, bg.bg_block_bitmap, i);
			if (used < 0)
				return -1;
			if (used)
				continue;

			if (ext2_bitmap_set(fs, bg.bg_block_bitmap, i) < 0)
				return -1;

			bg.bg_free_blocks_count--;
			if (ext2_save_bg(fs, g, &bg) < 0)
				return -1;

			fs->free_blocks--;
			*out = blk;
			return 0;
		}
	}

	return -1;
}

static int ext2_free_block(struct ext2_fs_priv* fs, uint32_t blk)
{
	uint32_t group = (blk - fs->first_data_block) / fs->blocks_per_group;
	uint32_t idx = (blk - fs->first_data_block) % fs->blocks_per_group;
	struct ext2_bg_desc bg;

	if (ext2_load_bg(fs, group, &bg) < 0)
		return -1;

	if (ext2_bitmap_clear(fs, bg.bg_block_bitmap, idx) < 0)
		return -1;

	bg.bg_free_blocks_count++;
	if (ext2_save_bg(fs, group, &bg) < 0)
		return -1;

	fs->free_blocks++;
	return 0;
}

/* 分配并清零一个块 */
static int ext2_alloc_zero_block(struct ext2_fs_priv* fs, uint32_t* out)
{
	uint8_t* zero;
	uint32_t blk;

	zero = slab_alloc(fs->block_size);
	if (zero == NULL)
		return -1;

	memset(zero, 0, fs->block_size);

	if (ext2_alloc_block(fs, &blk) < 0) {
		slab_free(zero);
		return -1;
	}

	if (ext2_write_block(fs, blk, zero) < 0) {
		ext2_free_block(fs, blk);
		slab_free(zero);
		return -1;
	}

	slab_free(zero);
	*out = blk;
	return 0;
}

/*
 * 块映射：逻辑块号 → 物理块号
 *  */

/* 间接块辅助：读写一个 ptrs_per_block 大小的指针数组 */
static int ext2_ind_get(struct ext2_fs_priv* fs,
			uint32_t ind_blk,
			uint32_t idx,
			uint32_t* out)
{
	uint8_t* buf;
	uint32_t v;

	buf = slab_alloc(fs->block_size);
	if (buf == NULL)
		return -1;

	if (ext2_read_block(fs, ind_blk, buf) < 0) {
		slab_free(buf);
		return -1;
	}

	memcpy(&v, buf + idx * 4, 4);
	slab_free(buf);
	*out = v;
	return 0;
}

static int ext2_ind_set(struct ext2_fs_priv* fs,
			uint32_t ind_blk,
			uint32_t idx,
			uint32_t val)
{
	uint8_t* buf;
	int ret;

	buf = slab_alloc(fs->block_size);
	if (buf == NULL)
		return -1;

	if (ext2_read_block(fs, ind_blk, buf) < 0) {
		slab_free(buf);
		return -1;
	}

	memcpy(buf + idx * 4, &val, 4);
	ret = ext2_write_block(fs, ind_blk, buf);

	slab_free(buf);
	return ret;
}

/*
 * 取/分配 inode 第 lblk 个逻辑块对应的物理块号。
 * allocate=0 时仅查询，遇到 hole 返回 0。
 * allocate=1 时遇到 hole 分配新块并清零，更新 inode。
 *
 * 调用者负责将修改后的 inode 回写到磁盘。
 *
 * 支持范围：
 *   0..11              → direct blocks
 *   12..(11+N)         → single indirect（N = ptrs_per_block）
 *   (12+N)..(12+N+N^2) → double indirect
 */
static int ext2_get_block(struct ext2_inode_priv* priv,
			  struct ext2_fs_priv* fs,
			  uint32_t lblk,
			  int allocate,
			  uint32_t* out)
{
	uint32_t ptrs = fs->ptrs_per_block;
	uint32_t bound_ind = 12 + ptrs;
	uint32_t bound_dind = bound_ind + ptrs * ptrs;

	if (lblk < 12) {
		if (priv->i_block[lblk] == 0 && allocate) {
			uint32_t nb;
			if (ext2_alloc_zero_block(fs, &nb) < 0)
				return -1;
			priv->i_block[lblk] = nb;
		}
		*out = priv->i_block[lblk];
		return 0;
	}

	if (lblk < bound_ind) {
		uint32_t idx = lblk - 12;
		uint32_t ind;

		if (priv->i_block[EXT2_IND_BLOCK] == 0) {
			if (!allocate) {
				*out = 0;
				return 0;
			}
			if (ext2_alloc_zero_block(fs, &ind) < 0)
				return -1;
			priv->i_block[EXT2_IND_BLOCK] = ind;
		}

		ind = priv->i_block[EXT2_IND_BLOCK];

		if (!allocate) {
			uint32_t v = 0;
			ext2_ind_get(fs, ind, idx, &v);
			*out = v;
			return 0;
		}

		/* allocate 路径：若该 slot 为 0 则分配 */
		uint32_t cur = 0;
		if (ext2_ind_get(fs, ind, idx, &cur) < 0)
			return -1;
		if (cur == 0) {
			uint32_t nb;
			if (ext2_alloc_zero_block(fs, &nb) < 0)
				return -1;
			if (ext2_ind_set(fs, ind, idx, nb) < 0)
				return -1;
			cur = nb;
		}
		*out = cur;
		return 0;
	}

	if (lblk < bound_dind) {
		uint32_t off = lblk - bound_ind;
		uint32_t idx1 = off / ptrs;
		uint32_t idx2 = off % ptrs;
		uint32_t dind;
		uint32_t ind = 0;

		if (priv->i_block[EXT2_DIND_BLOCK] == 0) {
			if (!allocate) {
				*out = 0;
				return 0;
			}
			if (ext2_alloc_zero_block(fs, &dind) < 0)
				return -1;
			priv->i_block[EXT2_DIND_BLOCK] = dind;
		}

		dind = priv->i_block[EXT2_DIND_BLOCK];

		if (ext2_ind_get(fs, dind, idx1, &ind) < 0)
			return -1;

		if (ind == 0) {
			if (!allocate) {
				*out = 0;
				return 0;
			}
			if (ext2_alloc_zero_block(fs, &ind) < 0)
				return -1;
			if (ext2_ind_set(fs, dind, idx1, ind) < 0)
				return -1;
		}

		if (!allocate) {
			uint32_t v = 0;
			ext2_ind_get(fs, ind, idx2, &v);
			*out = v;
			return 0;
		}

		uint32_t cur = 0;
		if (ext2_ind_get(fs, ind, idx2, &cur) < 0)
			return -1;
		if (cur == 0) {
			uint32_t nb;
			if (ext2_alloc_zero_block(fs, &nb) < 0)
				return -1;
			if (ext2_ind_set(fs, ind, idx2, nb) < 0)
				return -1;
			cur = nb;
		}
		*out = cur;
		return 0;
	}

	/* triple indirect 不支持 */
	return -1;
}

/* 释放 inode 的所有数据块（unlink/rmdir/truncate to 0 调用） */
static int ext2_free_all_blocks(struct ext2_fs_priv* fs,
				struct ext2_inode_priv* priv)
{
	uint32_t ptrs = fs->ptrs_per_block;
	uint8_t* buf = NULL;

	/* 释放 direct */
	for (uint32_t i = 0; i < 12; i++) {
		if (priv->i_block[i] != 0) {
			ext2_free_block(fs, priv->i_block[i]);
			priv->i_block[i] = 0;
		}
	}

	/* 释放 single indirect */
	if (priv->i_block[EXT2_IND_BLOCK] != 0) {
		uint32_t ind = priv->i_block[EXT2_IND_BLOCK];

		buf = slab_alloc(fs->block_size);
		if (buf == NULL)
			return -1;

		if (ext2_read_block(fs, ind, buf) < 0) {
			slab_free(buf);
			return -1;
		}

		for (uint32_t i = 0; i < ptrs; i++) {
			uint32_t v;
			memcpy(&v, buf + i * 4, 4);
			if (v != 0)
				ext2_free_block(fs, v);
		}

		ext2_free_block(fs, ind);
		priv->i_block[EXT2_IND_BLOCK] = 0;
		slab_free(buf);
		buf = NULL;
	}

	/* 释放 double indirect */
	if (priv->i_block[EXT2_DIND_BLOCK] != 0) {
		uint32_t dind = priv->i_block[EXT2_DIND_BLOCK];

		if (buf == NULL)
			buf = slab_alloc(fs->block_size);
		if (buf == NULL)
			return -1;

		if (ext2_read_block(fs, dind, buf) < 0) {
			slab_free(buf);
			return -1;
		}

		for (uint32_t i = 0; i < ptrs; i++) {
			uint32_t ind;
			memcpy(&ind, buf + i * 4, 4);
			if (ind == 0)
				continue;

			uint8_t* ibuf = slab_alloc(fs->block_size);
			if (ibuf == NULL)
				continue;

			if (ext2_read_block(fs, ind, ibuf) < 0) {
				slab_free(ibuf);
				continue;
			}

			for (uint32_t j = 0; j < ptrs; j++) {
				uint32_t v;
				memcpy(&v, ibuf + j * 4, 4);
				if (v != 0)
					ext2_free_block(fs, v);
			}

			ext2_free_block(fs, ind);
			slab_free(ibuf);
		}

		ext2_free_block(fs, dind);
		priv->i_block[EXT2_DIND_BLOCK] = 0;
		slab_free(buf);
	}

	return 0;
}

/*
 * 磁盘 inode 与 priv 同步
 *  */

/*
 * 重新统计 i_blocks：文件/目录占用的 512 字节扇区数。
 * 按 i_size 覆盖的逻辑块范围走一遍块映射，非 hole 即计数。
 * 在写、截断、建目录等改变数据块集合的路径调用。
 */
static void ext2_refresh_i_blocks(struct ext2_fs_priv* fs,
				  struct ext2_inode_priv* priv)
{
	uint32_t nblk = (priv->i_size + fs->block_size - 1) / fs->block_size;
	uint32_t used = 0;

	for (uint32_t l = 0; l < nblk; l++) {
		uint32_t phys = 0;
		if (ext2_get_block(priv, fs, l, 0, &phys) < 0)
			break;
		if (phys != 0)
			used++;
	}

	priv->i_blocks = used * (fs->block_size / 512);
}

static void ext2_priv_to_disk(const struct ext2_inode_priv* priv,
			      struct ext2_inode* out)
{
	memset(out, 0, sizeof(*out));
	out->i_mode = (uint16_t)priv->i_mode;
	out->i_uid = (uint16_t)priv->i_uid;
	out->i_size = priv->i_size;
	out->i_atime = priv->i_atime;
	out->i_ctime = priv->i_ctime;
	out->i_mtime = priv->i_mtime;
	out->i_gid = (uint16_t)priv->i_gid;
	out->i_links_count = (uint16_t)priv->i_links_count;
	out->i_blocks = priv->i_blocks;
	for (int i = 0; i < EXT2_N_BLOCKS; i++)
		out->i_block[i] = priv->i_block[i];
}

static void ext2_disk_to_priv(const struct ext2_inode* di,
			      uint32_t ino,
			      struct ext2_inode_priv* out)
{
	out->ino = ino;
	out->i_uid = di->i_uid;
	out->i_gid = di->i_gid;
	out->i_size = di->i_size;
	out->i_mode = di->i_mode;
	out->i_links_count = di->i_links_count;
	out->i_blocks = di->i_blocks;
	out->i_atime = di->i_atime;
	out->i_ctime = di->i_ctime;
	out->i_mtime = di->i_mtime;
	for (int i = 0; i < EXT2_N_BLOCKS; i++)
		out->i_block[i] = di->i_block[i];
}

/* 把 priv 回写到磁盘 inode */
static int ext2_priv_save(struct ext2_fs_priv* fs, struct ext2_inode_priv* priv)
{
	struct ext2_inode di;
	ext2_priv_to_disk(priv, &di);
	return ext2_inode_write(fs, priv->ino, &di);
}

/*
 * inode 构造
 *  */

static struct inode* ext2_inode_create(struct super_block* sb,
				       uint32_t ino,
				       const struct ext2_inode* di)
{
	struct ext2_fs_priv* fs = sb->private;
	struct inode* inode;
	struct ext2_inode_priv* priv;

	inode = slab_alloc(sizeof(*inode));
	priv = slab_alloc(sizeof(*priv));

	if (inode == NULL || priv == NULL) {
		if (inode)
			slab_free(inode);
		if (priv)
			slab_free(priv);
		return NULL;
	}

	memset(inode, 0, sizeof(*inode));
	memset(priv, 0, sizeof(*priv));

	ext2_disk_to_priv(di, ino, priv);

	inode->sb = sb;
	inode->ino = ino;
	inode->mode = priv->i_mode;
	inode->uid = priv->i_uid;
	inode->gid = priv->i_gid;
	inode->size = priv->i_size;
	inode->iops = &ext2_inode_ops;
	inode->fops = &ext2_file_ops;
	inode->private = priv;
	inode->refcount = 1;

	init_spinlock(&inode->lock);

	return inode;
}

static void ext2_inode_destroy(struct inode* inode)
{
	if (inode->private != NULL)
		slab_free(inode->private);
	slab_free(inode);
}

/*
 * 目录遍历
 *  */

/* 在 dir inode 中查找名为 name 的目录项，返回对应 inode number。 */
static int ext2_dir_find(struct ext2_fs_priv* fs,
			 struct ext2_inode_priv* dir,
			 const char* name,
			 size_t name_len,
			 uint32_t* out_ino)
{
	uint8_t* buf;
	uint32_t lblk = 0;
	uint32_t max_blk;

	buf = slab_alloc(fs->block_size);
	if (buf == NULL)
		return -1;

	max_blk = (dir->i_size + fs->block_size - 1) / fs->block_size;

	for (lblk = 0; lblk < max_blk; lblk++) {
		uint32_t phys = 0;
		if (ext2_get_block(dir, fs, lblk, 0, &phys) < 0) {
			slab_free(buf);
			return -1;
		}
		if (phys == 0)
			continue;

		if (ext2_read_block(fs, phys, buf) < 0) {
			slab_free(buf);
			return -1;
		}

		uint32_t off = 0;
		while (off + sizeof(struct ext2_dirent) <= fs->block_size) {
			struct ext2_dirent* e =
			    (struct ext2_dirent*)(buf + off);
			uint16_t rec = e->rec_len;
			if (rec < sizeof(struct ext2_dirent))
				break;

			if (e->inode != 0 && e->name_len == name_len &&
			    memcmp(e->name, name, name_len) == 0) {
				*out_ino = e->inode;
				slab_free(buf);
				return 0;
			}

			off += rec;
		}
	}

	slab_free(buf);
	return -1;
}

struct ext2_dir_scan {
	/* 输入：当前 offset cookie（lblk * block_size + off） */
	uint64_t offset;
	/* 输出：找到的目录项 */
	uint32_t ino;
	uint8_t type;
	char name[VFS_NAME_MAX + 1];
	uint32_t name_len;
	uint32_t next_off; /* 下一个 cookie */
	int found;
};

/* 按 offset 扫描目录，找到第一个有效目录项并返回。 */
static int ext2_dir_scan(struct ext2_fs_priv* fs,
			 struct ext2_inode_priv* dir,
			 struct ext2_dir_scan* out)
{
	uint8_t* buf;
	uint32_t start_blk = out->offset / fs->block_size;
	uint32_t max_blk;

	buf = slab_alloc(fs->block_size);
	if (buf == NULL)
		return -1;

	max_blk = (dir->i_size + fs->block_size - 1) / fs->block_size;

	for (uint32_t lblk = start_blk; lblk < max_blk; lblk++) {
		uint32_t phys = 0;
		uint32_t start_off =
		    (lblk == start_blk) ? out->offset % fs->block_size : 0;
		uint32_t off = start_off;

		if (ext2_get_block(dir, fs, lblk, 0, &phys) < 0) {
			slab_free(buf);
			return -1;
		}
		if (phys == 0)
			continue;

		if (ext2_read_block(fs, phys, buf) < 0) {
			slab_free(buf);
			return -1;
		}

		while (off + sizeof(struct ext2_dirent) <= fs->block_size) {
			struct ext2_dirent* e =
			    (struct ext2_dirent*)(buf + off);
			uint16_t rec = e->rec_len;
			if (rec < sizeof(struct ext2_dirent))
				break;

			/* 跳过 offset 之前已经看过的项 */
			if (off >= start_off) {
				if (e->inode != 0) {
					uint32_t nl = e->name_len;
					if (nl > VFS_NAME_MAX)
						nl = VFS_NAME_MAX;
					memcpy(out->name, e->name, nl);
					out->name[nl] = '\0';
					out->name_len = nl;
					out->ino = e->inode;
					out->type = e->file_type;
					out->next_off =
					    lblk * fs->block_size + off + rec;
					out->found = 1;
					slab_free(buf);
					return 0;
				}
			}

			off += rec;
		}
	}

	slab_free(buf);
	out->found = 0;
	return 0;
}

/* 在 dir 中追加一个目录项。
 * 策略：先扫所有块找空闲槽位（inode==0 且 rec_len 足够大）；
 * 找不到则扩展目录一个数据块，把新项放在块开头。 */
static int ext2_dir_add(struct ext2_fs_priv* fs,
			struct ext2_inode_priv* dir,
			const char* name,
			size_t name_len,
			uint32_t ino,
			uint8_t file_type)
{
	uint8_t* buf;
	uint32_t max_blk;
	uint16_t need =
	    (uint16_t)((sizeof(struct ext2_dirent) + name_len + 3) & ~3u);
	uint32_t lblk;
	uint32_t added = 0;

	buf = slab_alloc(fs->block_size);
	if (buf == NULL)
		return -1;

	max_blk = (dir->i_size + fs->block_size - 1) / fs->block_size;

	/* 1. 尝试在现有块里找空闲槽位或拆分最后一个 entry */
	for (lblk = 0; lblk < max_blk && !added; lblk++) {
		uint32_t phys = 0;
		if (ext2_get_block(dir, fs, lblk, 0, &phys) < 0) {
			slab_free(buf);
			return -1;
		}
		if (phys == 0)
			continue;

		if (ext2_read_block(fs, phys, buf) < 0) {
			slab_free(buf);
			return -1;
		}

		uint32_t off = 0;
		uint32_t last_off = 0;
		uint16_t last_rec = 0;

		while (off + sizeof(struct ext2_dirent) <= fs->block_size) {
			struct ext2_dirent* e =
			    (struct ext2_dirent*)(buf + off);
			if (e->rec_len < sizeof(struct ext2_dirent))
				break;

			last_off = off;
			last_rec = e->rec_len;

			/* 空闲槽位（inode==0）的复用：要求 rec_len >= need */
			if (e->inode == 0 && e->rec_len >= need) {
				/* 把这个空闲槽位一分为二：
				 * 头部写入新项，尾部仍作为空闲槽位 */
				uint16_t rest = e->rec_len - need;
				if (rest >=
				    (uint16_t)sizeof(struct ext2_dirent)) {
					/* 分割：尾部继续作为 inode=0 的空闲项
					 */
					e->rec_len = need;
					struct ext2_dirent* restp =
					    (struct ext2_dirent*)(buf + off +
								  need);
					restp->inode = 0;
					restp->rec_len = rest;
					restp->name_len = 0;
					restp->file_type = 0;
				}
				/* 否则把整个槽位直接覆盖 */

				e->inode = ino;
				e->name_len = (uint8_t)name_len;
				e->file_type = file_type;
				memcpy(e->name, name, name_len);
				added = 1;
				break;
			}

			off += e->rec_len;
		}

		if (!added) {
			/* 尝试在最后一个 entry 后追加，前提是它有富余空间 */
			if (last_rec > 0) {
				/* 计算 last entry 真实长度 */
				struct ext2_dirent* last =
				    (struct ext2_dirent*)(buf + last_off);
				uint16_t real_len =
				    (uint16_t)((sizeof(struct ext2_dirent) +
						last->name_len + 3) &
					       ~3u);
				uint16_t tail = last_rec - real_len;
				if (tail >= need) {
					last->rec_len = real_len;
					struct ext2_dirent* ne =
					    (struct ext2_dirent*)(buf +
								  last_off +
								  real_len);
					ne->inode = ino;
					ne->rec_len = tail;
					ne->name_len = (uint8_t)name_len;
					ne->file_type = file_type;
					memcpy(ne->name, name, name_len);
					added = 1;
				}
			}
		}

		if (added) {
			if (ext2_write_block(fs, phys, buf) < 0) {
				slab_free(buf);
				return -1;
			}
		}
	}

	/* 2. 现有块没有空间，扩展一个新数据块 */
	if (!added) {
		uint32_t new_phys = 0;
		uint32_t new_lblk = max_blk;

		if (ext2_get_block(dir, fs, new_lblk, 1, &new_phys) < 0) {
			slab_free(buf);
			return -1;
		}
		if (new_phys == 0) {
			slab_free(buf);
			return -1;
		}

		memset(buf, 0, fs->block_size);

		struct ext2_dirent* ne = (struct ext2_dirent*)buf;
		ne->inode = ino;
		ne->rec_len = (uint16_t)fs->block_size;
		ne->name_len = (uint8_t)name_len;
		ne->file_type = file_type;
		memcpy(ne->name, name, name_len);

		if (ext2_write_block(fs, new_phys, buf) < 0) {
			slab_free(buf);
			return -1;
		}

		dir->i_size = (new_lblk + 1) * fs->block_size;
		added = 1;
	}

	slab_free(buf);
	return added ? 0 : -1;
}

/* 在 dir 中删除名为 name 的目录项：把对应 entry 的 inode 置 0。 */
static int ext2_dir_remove(struct ext2_fs_priv* fs,
			   struct ext2_inode_priv* dir,
			   const char* name,
			   size_t name_len)
{
	uint8_t* buf;
	uint32_t max_blk;

	buf = slab_alloc(fs->block_size);
	if (buf == NULL)
		return -1;

	max_blk = (dir->i_size + fs->block_size - 1) / fs->block_size;

	for (uint32_t lblk = 0; lblk < max_blk; lblk++) {
		uint32_t phys = 0;
		if (ext2_get_block(dir, fs, lblk, 0, &phys) < 0) {
			slab_free(buf);
			return -1;
		}
		if (phys == 0)
			continue;

		if (ext2_read_block(fs, phys, buf) < 0) {
			slab_free(buf);
			return -1;
		}

		uint32_t off = 0;
		while (off + sizeof(struct ext2_dirent) <= fs->block_size) {
			struct ext2_dirent* e =
			    (struct ext2_dirent*)(buf + off);
			if (e->rec_len < sizeof(struct ext2_dirent))
				break;

			if (e->inode != 0 && e->name_len == name_len &&
			    memcmp(e->name, name, name_len) == 0) {
				/* 与前一项合并：把 rec_len 加给前一项。
				 * 若没有前一项（off==0），仅置 inode=0。 */
				if (off == 0) {
					e->inode = 0;
				} else {
					/* 找上一个 entry：从头扫到上一个 off
					 * 之前的累计 */
					uint32_t p = 0;
					while (p < off) {
						struct ext2_dirent* pe =
						    (struct ext2_dirent*)(buf +
									  p);
						uint32_t next = p + pe->rec_len;
						if (next == off) {
							pe->rec_len +=
							    e->rec_len;
							break;
						}
						p = next;
					}
					e->inode = 0;
				}

				if (ext2_write_block(fs, phys, buf) < 0) {
					slab_free(buf);
					return -1;
				}

				slab_free(buf);
				return 0;
			}

			off += e->rec_len;
		}
	}

	slab_free(buf);
	return -1;
}

/* 检查目录是否为空（除 "." 和 ".." 外无其他有效项） */
static int ext2_dir_empty(struct ext2_fs_priv* fs, struct ext2_inode_priv* dir)
{
	uint8_t* buf;
	uint32_t max_blk;

	buf = slab_alloc(fs->block_size);
	if (buf == NULL)
		return -1;

	max_blk = (dir->i_size + fs->block_size - 1) / fs->block_size;

	for (uint32_t lblk = 0; lblk < max_blk; lblk++) {
		uint32_t phys = 0;
		if (ext2_get_block(dir, fs, lblk, 0, &phys) < 0) {
			slab_free(buf);
			return -1;
		}
		if (phys == 0)
			continue;

		if (ext2_read_block(fs, phys, buf) < 0) {
			slab_free(buf);
			return -1;
		}

		uint32_t off = 0;
		while (off + sizeof(struct ext2_dirent) <= fs->block_size) {
			struct ext2_dirent* e =
			    (struct ext2_dirent*)(buf + off);
			if (e->rec_len < sizeof(struct ext2_dirent))
				break;

			if (e->inode != 0) {
				/* 跳过 "." 和 ".." */
				if (!(e->name_len == 1 && e->name[0] == '.') &&
				    !(e->name_len == 2 && e->name[0] == '.' &&
				      e->name[1] == '.')) {
					slab_free(buf);
					return 0; /* 非空 */
				}
			}

			off += e->rec_len;
		}
	}

	slab_free(buf);
	return 1; /* 空 */
}

/* 在 dir 中创建 "." 和 ".." */
static int ext2_dir_init_dot(struct ext2_fs_priv* fs,
			     struct ext2_inode_priv* dir,
			     uint32_t parent_ino)
{
	uint8_t* buf;
	uint32_t phys = 0;

	if (ext2_get_block(dir, fs, 0, 1, &phys) < 0 || phys == 0)
		return -1;

	buf = slab_alloc(fs->block_size);
	if (buf == NULL)
		return -1;

	memset(buf, 0, fs->block_size);

	struct ext2_dirent* dot = (struct ext2_dirent*)buf;
	dot->inode = dir->ino;
	dot->rec_len = 12;
	dot->name_len = 1;
	dot->file_type = EXT2_FT_DIR;
	dot->name[0] = '.';

	struct ext2_dirent* ddot = (struct ext2_dirent*)(buf + 12);
	ddot->inode = parent_ino;
	ddot->rec_len = (uint16_t)(fs->block_size - 12);
	ddot->name_len = 2;
	ddot->file_type = EXT2_FT_DIR;
	ddot->name[0] = '.';
	ddot->name[1] = '.';

	int ret = ext2_write_block(fs, phys, buf);

	slab_free(buf);

	dir->i_size = fs->block_size;

	return ret;
}

/*
 * inode_operations
 *  */

static int ext2_lookup(struct inode* dir, const char* name, struct inode** out)
{
	struct ext2_fs_priv* fs;
	struct ext2_inode_priv* dp;
	uint32_t ino = 0;
	struct ext2_inode di;
	struct inode* ni;
	size_t name_len;

	if (dir == NULL || name == NULL || out == NULL || dir->sb == NULL ||
	    dir->sb->private == NULL || dir->private == NULL)
		return -1;

	if ((dir->mode & S_IFMT) != S_IFDIR)
		return -1;

	/* VFS 层负责消解 "." ".."，这里不应该收到 */
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
		return -1;

	fs = dir->sb->private;
	dp = dir->private;

	name_len = strlen(name);
	if (name_len > VFS_NAME_MAX)
		return -1;

	if (ext2_dir_find(fs, dp, name, name_len, &ino) < 0)
		return -1;

	if (ext2_inode_read(fs, ino, &di) < 0)
		return -1;

	ni = ext2_inode_create(dir->sb, ino, &di);
	if (ni == NULL)
		return -1;

	*out = ni;
	return 0;
}

static int ext2_create(struct inode* dir,
		       const char* name,
		       uint32_t mode,
		       struct inode** inode)
{
	struct ext2_fs_priv* fs;
	struct ext2_inode_priv* dp;
	uint32_t new_ino;
	struct ext2_inode di;
	uint32_t now;
	size_t name_len;

	if (dir == NULL || name == NULL || inode == NULL || dir->sb == NULL ||
	    dir->sb->private == NULL || dir->private == NULL)
		return -1;

	/* 调用方可能只传权限位（如 0666），不带 S_IFREG 类型位；
	 * mode 的 S_IFMT 全 0 时自动补 S_IFREG */
	if ((mode & S_IFMT) == 0)
		mode |= S_IFREG;
	if ((mode & S_IFMT) != S_IFREG)
		return -1;

	fs = dir->sb->private;
	dp = dir->private;

	name_len = strlen(name);
	if (name_len == 0 || name_len > VFS_NAME_MAX)
		return -1;

	/* 已存在则失败 */
	{
		uint32_t exist = 0;
		if (ext2_dir_find(fs, dp, name, name_len, &exist) == 0)
			return -1;
	}

	if (ext2_alloc_inode(fs, &new_ino) < 0)
		return -1;

	now = ext2_now();

	memset(&di, 0, sizeof(di));
	di.i_mode = (uint16_t)(EXT2_S_IFREG | (mode & 0777));
	di.i_uid = 0;
	di.i_gid = 0;
	di.i_size = 0;
	di.i_atime = now;
	di.i_ctime = now;
	di.i_mtime = now;
	di.i_links_count = 1;

	if (ext2_inode_write(fs, new_ino, &di) < 0) {
		ext2_free_inode(fs, new_ino);
		return -1;
	}

	/* 把目录项加入父目录 */
	if (ext2_dir_add(fs, dp, name, name_len, new_ino, EXT2_FT_REG_FILE) <
	    0) {
		ext2_free_inode(fs, new_ino);
		return -1;
	}

	/* 父目录的 mtime/ctime 更新；dir_add 可能扩展了目录块 */
	dp->i_mtime = now;
	dp->i_ctime = now;
	ext2_refresh_i_blocks(fs, dp);
	ext2_priv_save(fs, dp);

	/* 构造 VFS inode */
	struct inode* ni = ext2_inode_create(dir->sb, new_ino, &di);
	if (ni == NULL) {
		ext2_free_inode(fs, new_ino);
		return -1;
	}

	ext2_cache_flush(fs);
	*inode = ni;
	return 0;
}

static int ext2_mkdir(struct inode* dir,
		      const char* name,
		      uint32_t mode,
		      struct inode** inode)
{
	struct ext2_fs_priv* fs;
	struct ext2_inode_priv* dp;
	uint32_t new_ino;
	struct ext2_inode di;
	uint32_t now;
	size_t name_len;
	struct inode* ni;
	struct ext2_inode_priv* np;

	if (dir == NULL || name == NULL || inode == NULL || dir->sb == NULL ||
	    dir->sb->private == NULL || dir->private == NULL)
		return -1;

	if ((mode & S_IFMT) != S_IFDIR)
		return -1;

	fs = dir->sb->private;
	dp = dir->private;

	name_len = strlen(name);
	if (name_len == 0 || name_len > VFS_NAME_MAX)
		return -1;

	{
		uint32_t exist = 0;
		if (ext2_dir_find(fs, dp, name, name_len, &exist) == 0)
			return -1;
	}

	if (ext2_alloc_inode(fs, &new_ino) < 0)
		return -1;

	now = ext2_now();

	memset(&di, 0, sizeof(di));
	di.i_mode = (uint16_t)(EXT2_S_IFDIR | (mode & 0777));
	di.i_uid = 0;
	di.i_gid = 0;
	di.i_size = fs->block_size;
	di.i_atime = now;
	di.i_ctime = now;
	di.i_mtime = now;
	di.i_links_count = 2; /* "." + 父项 */

	if (ext2_inode_write(fs, new_ino, &di) < 0) {
		ext2_free_inode(fs, new_ino);
		return -1;
	}

	/* 在父目录中加入目录项 */
	if (ext2_dir_add(fs, dp, name, name_len, new_ino, EXT2_FT_DIR) < 0) {
		ext2_free_inode(fs, new_ino);
		return -1;
	}

	/* 给新目录建立 "." 和 ".." */
	ni = ext2_inode_create(dir->sb, new_ino, &di);
	if (ni == NULL) {
		ext2_free_inode(fs, new_ino);
		return -1;
	}
	np = ni->private;

	if (ext2_dir_init_dot(fs, np, dp->ino) < 0) {
		inode_put(ni);
		/* 回滚目录项 */
		ext2_dir_remove(fs, dp, name, name_len);
		ext2_free_inode(fs, new_ino);
		return -1;
	}

	/* 新目录已分配第一个数据块，刷新扇区计数 */
	ext2_refresh_i_blocks(fs, np);

	if (ext2_priv_save(fs, np) < 0) {
		inode_put(ni);
		ext2_dir_remove(fs, dp, name, name_len);
		ext2_free_inode(fs, new_ino);
		return -1;
	}

	/* 父目录 links_count++（因为 ".." 引用）；dir_add 可能扩展了目录块 */
	dp->i_links_count++;
	dp->i_mtime = now;
	dp->i_ctime = now;
	ext2_refresh_i_blocks(fs, dp);
	ext2_priv_save(fs, dp);

	ext2_cache_flush(fs);
	*inode = ni;
	return 0;
}

static int ext2_unlink(struct inode* dir, const char* name)
{
	struct ext2_fs_priv* fs;
	struct ext2_inode_priv* dp;
	uint32_t ino;
	struct ext2_inode di;
	struct ext2_inode_priv target_priv;
	size_t name_len;
	uint32_t now;

	if (dir == NULL || name == NULL || dir->sb == NULL ||
	    dir->sb->private == NULL || dir->private == NULL)
		return -1;

	fs = dir->sb->private;
	dp = dir->private;

	name_len = strlen(name);
	if (name_len == 0 || name_len > VFS_NAME_MAX)
		return -1;

	if (ext2_dir_find(fs, dp, name, name_len, &ino) < 0)
		return -1;

	if (ext2_inode_read(fs, ino, &di) < 0)
		return -1;

	/* 不能对目录调用 unlink */
	if ((di.i_mode & S_IFMT) == S_IFDIR)
		return -1;

	/* 删除目录项 */
	if (ext2_dir_remove(fs, dp, name, name_len) < 0)
		return -1;

	/* 释放数据块 */
	ext2_disk_to_priv(&di, ino, &target_priv);
	ext2_free_all_blocks(fs, &target_priv);

	/* links_count-- ，若到 0 则释放 inode */
	if (di.i_links_count > 0)
		di.i_links_count--;

	di.i_dtime = ext2_now();

	if (ext2_inode_write(fs, ino, &di) < 0)
		return -1;

	if (di.i_links_count == 0) {
		ext2_free_inode(fs, ino);
	}

	now = ext2_now();
	dp->i_mtime = now;
	dp->i_ctime = now;
	ext2_priv_save(fs, dp);

	ext2_cache_flush(fs);
	return 0;
}

static int ext2_rmdir(struct inode* dir, const char* name)
{
	struct ext2_fs_priv* fs;
	struct ext2_inode_priv* dp;
	uint32_t ino;
	struct ext2_inode di;
	struct ext2_inode_priv target_priv;
	size_t name_len;
	uint32_t now;

	if (dir == NULL || name == NULL || dir->sb == NULL ||
	    dir->sb->private == NULL || dir->private == NULL)
		return -1;

	fs = dir->sb->private;
	dp = dir->private;

	name_len = strlen(name);
	if (name_len == 0 || name_len > VFS_NAME_MAX)
		return -1;

	if (ext2_dir_find(fs, dp, name, name_len, &ino) < 0)
		return -1;

	if (ext2_inode_read(fs, ino, &di) < 0)
		return -1;

	if ((di.i_mode & S_IFMT) != S_IFDIR)
		return -1;

	ext2_disk_to_priv(&di, ino, &target_priv);

	/* 目录必须为空（links_count <= 2 表示只有 "." 和 ".."） */
	if (di.i_links_count > 2)
		return -1;

	/* 防御性检查：扫描目录项确认确实没有非 . .. 的项 */
	if (ext2_dir_empty(fs, &target_priv) != 1)
		return -1;

	/* 删除目录项 */
	if (ext2_dir_remove(fs, dp, name, name_len) < 0)
		return -1;

	/* 释放数据块 */
	ext2_free_all_blocks(fs, &target_priv);

	/* 释放 inode */
	di.i_dtime = ext2_now();
	di.i_links_count = 0;
	if (ext2_inode_write(fs, ino, &di) < 0)
		return -1;

	ext2_free_inode(fs, ino);

	/* 父目录 links_count-- */
	if (dp->i_links_count > 0)
		dp->i_links_count--;

	now = ext2_now();
	dp->i_mtime = now;
	dp->i_ctime = now;
	ext2_priv_save(fs, dp);

	ext2_cache_flush(fs);
	return 0;
}

static int
ext2_readdir(struct inode* dir, uint64_t* offset, struct vfs_dirent* dirent)
{
	struct ext2_fs_priv* fs;
	struct ext2_inode_priv* dp;
	struct ext2_dir_scan scan;

	if (dir == NULL || offset == NULL || dirent == NULL ||
	    dir->sb == NULL || dir->sb->private == NULL || dir->private == NULL)
		return -1;

	if ((dir->mode & S_IFMT) != S_IFDIR)
		return -1;

	fs = dir->sb->private;
	dp = dir->private;

	memset(&scan, 0, sizeof(scan));
	scan.offset = *offset;

	if (ext2_dir_scan(fs, dp, &scan) < 0)
		return -1;

	if (!scan.found)
		return -1;

	dirent->ino = scan.ino;
	dirent->type = (scan.type == EXT2_FT_DIR)	 ? S_IFDIR
		       : (scan.type == EXT2_FT_REG_FILE) ? S_IFREG
		       : (scan.type == EXT2_FT_SYMLINK)	 ? S_IFLNK
							 : S_IFREG;
	memcpy(dirent->name, scan.name, scan.name_len);
	dirent->name[scan.name_len] = '\0';

	*offset = scan.next_off;
	return 0;
}

static int ext2_getattr(struct inode* inode, struct vfs_kstat* stat)
{
	struct ext2_inode_priv* priv;

	if (inode == NULL || stat == NULL || inode->sb == NULL ||
	    inode->private == NULL)
		return -1;

	priv = inode->private;

	memset(stat, 0, sizeof(*stat));
	stat->ino = inode->ino;
	stat->mode = inode->mode;
	stat->uid = inode->uid;
	stat->gid = inode->gid;
	stat->size = inode->size;
	stat->nlink = priv->i_links_count;
	stat->blocks = (inode->size + 511) / 512;
	stat->blksize = ((struct ext2_fs_priv*)inode->sb->private)->block_size;
	stat->atime = priv->i_atime;
	stat->mtime = priv->i_mtime;
	stat->ctime = priv->i_ctime;

	return 0;
}

static int ext2_truncate(struct inode* inode, uint64_t size)
{
	struct ext2_fs_priv* fs;
	struct ext2_inode_priv* priv;
	uint32_t old_blocks;
	uint32_t new_blocks;

	if (inode == NULL || inode->sb == NULL || inode->sb->private == NULL ||
	    inode->private == NULL)
		return -1;

	if ((inode->mode & S_IFMT) != S_IFREG)
		return -1;

	fs = inode->sb->private;
	priv = inode->private;

	old_blocks = (priv->i_size + fs->block_size - 1) / fs->block_size;
	new_blocks = (uint32_t)((size + fs->block_size - 1) / fs->block_size);

	if (new_blocks < old_blocks) {
		/* 释放多余块 */
		for (uint32_t lblk = new_blocks; lblk < old_blocks; lblk++) {
			uint32_t phys = 0;
			if (ext2_get_block(priv, fs, lblk, 0, &phys) < 0)
				return -1;
			if (phys != 0) {
				ext2_free_block(fs, phys);
				/* 把映射清空：直接覆盖 i_block / 间接项 */
				/* 简化处理：只在 direct 区段清空。
				 * 间接区段的清空在 free_all_blocks 里做。 */
				if (lblk < 12)
					priv->i_block[lblk] = 0;
			}
		}

		/* 如果是大幅截断到 0，直接释放所有块 */
		if (new_blocks == 0) {
			ext2_free_all_blocks(fs, priv);
		}
	}
	/* 扩展到更大 size：不预分配块，write 时按需分配（产生 sparse hole） */

	priv->i_size = (uint32_t)size;
	priv->i_mtime = ext2_now();
	priv->i_ctime = ext2_now();

	inode->size = size;

	ext2_refresh_i_blocks(fs, priv);

	if (ext2_priv_save(fs, priv) < 0)
		return -1;
	ext2_cache_flush(fs);
	return 0;
}

/*
 * 数据读写
 *  */

static int ext2_read_data(struct inode* inode,
			  uint64_t pos,
			  void* buf,
			  uint32_t count,
			  uint32_t* out_len)
{
	struct ext2_fs_priv* fs = inode->sb->private;
	struct ext2_inode_priv* priv = inode->private;
	uint8_t* scratch;
	uint8_t* out = buf;
	uint64_t remaining;
	uint64_t avail;

	if (pos >= inode->size) {
		*out_len = 0;
		return 0;
	}

	avail = inode->size - pos;
	if (count > avail)
		count = avail;

	scratch = slab_alloc(fs->block_size);
	if (scratch == NULL)
		return -1;

	remaining = count;

	while (remaining > 0) {
		uint32_t lblk = (uint32_t)(pos / fs->block_size);
		uint32_t boff = (uint32_t)(pos % fs->block_size);
		uint32_t n =
		    (uint32_t)(remaining < (uint64_t)(fs->block_size - boff)
				   ? remaining
				   : (fs->block_size - boff));
		uint32_t phys = 0;

		if (ext2_get_block(priv, fs, lblk, 0, &phys) < 0) {
			slab_free(scratch);
			return -1;
		}

		if (phys == 0) {
			/* hole：返回 0 */
			memset(out, 0, n);
		} else {
			if (ext2_read_block(fs, phys, scratch) < 0) {
				slab_free(scratch);
				return -1;
			}
			memcpy(out, scratch + boff, n);
		}

		out += n;
		pos += n;
		remaining -= n;
	}

	slab_free(scratch);
	*out_len = count - (uint32_t)remaining;
	return 0;
}

static int ext2_write_data(struct inode* inode,
			   uint64_t pos,
			   const void* buf,
			   uint32_t count,
			   uint32_t* out_len)
{
	struct ext2_fs_priv* fs = inode->sb->private;
	struct ext2_inode_priv* priv = inode->private;
	uint8_t* scratch;
	const uint8_t* ub = buf;
	uint64_t end = pos + count;
	uint64_t f = pos;
	int dirty_inode = 0;

	scratch = slab_alloc(fs->block_size);
	if (scratch == NULL)
		return -1;

	while (f < end) {
		uint32_t lblk = (uint32_t)(f / fs->block_size);
		uint32_t boff = (uint32_t)(f % fs->block_size);
		uint32_t n =
		    (uint32_t)((end - f) < (uint64_t)(fs->block_size - boff)
				   ? (end - f)
				   : (fs->block_size - boff));
		uint32_t phys = 0;

		if (ext2_get_block(priv, fs, lblk, 1, &phys) < 0) {
			slab_free(scratch);
			return -1;
		}
		if (phys == 0) {
			slab_free(scratch);
			return -1;
		}

		if (boff == 0 && n == fs->block_size) {
			/* 整块写 */
			if (ext2_write_block(fs, phys, ub + (f - pos)) < 0) {
				slab_free(scratch);
				return -1;
			}
		} else {
			/* 读-改-写 */
			if (ext2_read_block(fs, phys, scratch) < 0) {
				slab_free(scratch);
				return -1;
			}
			memcpy(scratch + boff, ub + (f - pos), n);
			if (ext2_write_block(fs, phys, scratch) < 0) {
				slab_free(scratch);
				return -1;
			}
		}

		f += n;
		dirty_inode = 1;
	}

	if (end > inode->size) {
		priv->i_size = (uint32_t)end;
		inode->size = end;
		dirty_inode = 1;
	}

	priv->i_mtime = ext2_now();
	priv->i_ctime = priv->i_mtime;

	if (dirty_inode) {
		ext2_refresh_i_blocks(fs, priv);

		if (ext2_priv_save(fs, priv) < 0) {
			slab_free(scratch);
			return -1;
		}
	}

	slab_free(scratch);
	*out_len = count;
	return 0;
}

/*
 * file_operations
 *  */

static int64_t ext2_read(struct file* file, void* buf, uint64_t count)
{
	struct inode* inode;
	uint32_t out_len = 0;

	if (file == NULL || file->inode == NULL || buf == NULL)
		return -1;

	inode = file->inode;

	if ((inode->mode & S_IFMT) != S_IFREG)
		return -1;

	if (count == 0)
		return 0;

	if (count > 0xFFFFFFFFULL)
		count = 0xFFFFFFFFULL;

	if (ext2_read_data(inode, file->pos, buf, count, &out_len) < 0)
		return -1;

	return out_len;
}

static int64_t ext2_write(struct file* file, const void* buf, uint64_t count)
{
	struct inode* inode;
	uint32_t out_len = 0;

	if (file == NULL || file->inode == NULL || buf == NULL)
		return -1;

	inode = file->inode;

	if ((inode->mode & S_IFMT) != S_IFREG)
		return -1;

	if (count == 0)
		return 0;

	if (count > 0xFFFFFFFFULL)
		count = 0xFFFFFFFFULL;

	if (ext2_write_data(inode, file->pos, buf, count, &out_len) < 0)
		return -1;

	ext2_cache_flush(inode->sb->private);
	return out_len;
}

static int64_t ext2_seek(struct file* file, int64_t offset, int whence)
{
	struct inode* inode;
	int64_t npos;

	if (file == NULL || file->inode == NULL)
		return -1;

	inode = file->inode;

	switch (whence) {
	case SEEK_SET:
		npos = offset;
		break;
	case SEEK_CUR:
		npos = (int64_t)file->pos + offset;
		break;
	case SEEK_END:
		npos = (int64_t)inode->size + offset;
		break;
	default:
		return -1;
	}

	if (npos < 0)
		return -1;

	/* 允许 seek 超过 size，用于在写时创建 hole */
	return npos;
}

static int ext2_close(struct file* file)
{
	(void)file;
	return 0;
}

/*
 * super_block
 *  */

static int ext2_get_super(struct filesystem* fs,
			  struct block_device* dev,
			  struct super_block** out)
{
	struct ext2_superblock sb;
	struct ext2_fs_priv* priv;
	struct super_block* sb_obj;
	struct inode* root;
	struct ext2_inode root_di;
	uint32_t block_size;
	uint32_t first_data_block;
	uint32_t num_groups;
	int ret = -1;

	if (fs == NULL || dev == NULL || out == NULL)
		return -1;

	/* 读取超级块（字节 1024 处，2 个扇区） */
	{
		uint8_t sbuf[1024];
		if (dev->driver.read(dev->private_data, 2, sbuf) < 0)
			return -1;
		if (dev->driver.read(dev->private_data, 3, sbuf + 512) < 0)
			return -1;
		memcpy(&sb, sbuf, sizeof(sb));
	}

	if (sb.s_magic != EXT2_MAGIC)
		return -1;

	if (sb.s_rev_level > EXT2_REV_LEVEL_1)
		return -1;

	block_size = 1024u << sb.s_log_block_size;
	if (block_size > EXT2_MAX_BLOCK_SIZE)
		return -1;

	first_data_block = (block_size == 1024) ? 1 : 0;
	num_groups = (sb.s_blocks_count - sb.s_first_data_block +
		      sb.s_blocks_per_group - 1) /
		     sb.s_blocks_per_group;

	priv = slab_alloc(sizeof(*priv));
	if (priv == NULL)
		return -1;

	memset(priv, 0, sizeof(*priv));

	priv->bdev = dev;
	priv->block_size = block_size;
	priv->inodes_per_group = sb.s_inodes_per_group;
	priv->blocks_per_group = sb.s_blocks_per_group;
	/*
	 * 磁盘 inode 大小：
	 *   rev 0 固定 128；rev 1 由 s_inode_size（超级块偏移 88）给出，
	 *   mkfs.ext2 在 2K/4K 块上通常写 256。
	 * 绝不能硬编码 128——否则 inode 表中 idx>0 的 inode 定位全部错位，
	 * root inode (ino=2, idx=1) 会读到自身记录的中段(i_block 区)，
	 * i_mode 变成 0，被 VFS 判为"非目录"。
	 */
	if (sb.s_rev_level == EXT2_REV_LEVEL_0 || sb.s_inode_size == 0)
		priv->inode_size = EXT2_INODE_SIZE_DEFAULT;
	else
		priv->inode_size = sb.s_inode_size;

	if (priv->inode_size < EXT2_INODE_SIZE_DEFAULT ||
	    priv->inode_size > block_size ||
	    (priv->inode_size & (priv->inode_size - 1)) != 0) {
		slab_free(priv);
		return -1;
	}
	priv->num_groups = num_groups;
	priv->inodes_count = sb.s_inodes_count;
	priv->blocks_count = sb.s_blocks_count;
	priv->free_inodes = sb.s_free_inodes_count;
	priv->free_blocks = sb.s_free_blocks_count;
	priv->first_data_block = first_data_block;
	priv->ptrs_per_block = block_size / 4;
	priv->s_state = sb.s_state;
	priv->s_rev_level = sb.s_rev_level;

	/*
	 * 在第一次 ext2_read_block / ext2_write_block（即 ext2_inode_read）
	 * 之前初始化块缓存。之后所有块访问都走缓存。
	 */
	if (ext2_cache_init(priv) < 0) {
		slab_free(priv);
		return -1;
	}

	sb_obj = slab_alloc(sizeof(*sb_obj));
	if (sb_obj == NULL) {
		ext2_cache_destroy(priv);
		slab_free(priv);
		return -1;
	}

	memset(sb_obj, 0, sizeof(*sb_obj));

	sb_obj->fs = fs;
	sb_obj->dev = dev;
	sb_obj->private = priv;

	if (ext2_inode_read(priv, EXT2_ROOT_INO, &root_di) < 0) {
		slab_free(sb_obj);
		ext2_cache_destroy(priv);
		slab_free(priv);
		return -1;
	}

	root = ext2_inode_create(sb_obj, EXT2_ROOT_INO, &root_di);
	if (root == NULL) {
		slab_free(sb_obj);
		ext2_cache_destroy(priv);
		slab_free(priv);
		return -1;
	}

	/*
	 * 挂载阶段自检：root 必须是目录。
	 * 若磁盘参数解析错误（如 s_inode_size 取错导致 inode 表错位），
	 * 这里就会失败，而不是拖到 set_cwd("/") 才以
	 * "inode 不是目录"的形式暴露。
	 */
	if ((root->mode & S_IFMT) != S_IFDIR) {
		inode_put(root);
		slab_free(sb_obj);
		ext2_cache_destroy(priv);
		slab_free(priv);
		return -1;
	}

	sb_obj->root = root;
	*out = sb_obj;
	ret = 0;

	return ret;
}

static void ext2_kill_sb(struct super_block* sb)
{
	if (sb == NULL)
		return;

	inode_put(sb->root);

	if (sb->private != NULL) {
		/* 卸载前把所有 dirty 块落盘，再释放缓存结构 */
		ext2_cache_flush(sb->private);
		ext2_cache_destroy(sb->private);
		slab_free(sb->private);
	}

	slab_free(sb);
}

/*
 * 注册
 *  */

static struct filesystem ext2_fs = {
    .name = "ext2",
    .get_super = ext2_get_super,
    .kill_sb = ext2_kill_sb,
    .fops = &ext2_file_ops,
    .iops = &ext2_inode_ops,
};

static struct inode_operations ext2_inode_ops = {
    .lookup = ext2_lookup,
    .create = ext2_create,
    .mkdir = ext2_mkdir,
    .rmdir = ext2_rmdir,
    .unlink = ext2_unlink,
    .readdir = ext2_readdir,
    .getattr = ext2_getattr,
    .truncate = ext2_truncate,
    .destroy = ext2_inode_destroy,
};

static struct file_operations ext2_file_ops = {
    .read = ext2_read,
    .write = ext2_write,
    .seek = ext2_seek,
    .close = ext2_close,
};

void init_ext2(void)
{
	if (vfs_register_filesystem(&ext2_fs) < 0)
		printk("ext2: register filesystem failed\n");
	else
		printk("ext2: filesystem registered\n");
}
