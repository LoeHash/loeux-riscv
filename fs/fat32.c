#include <fat32.h>
#include <memory.h>
#include <block_device.h>
#include <printk.h>
#include <vfs.h>
#include <lib.h>
#include <timer.h>
#include <slab.h>
#include <spinlock.h>

/*
 * FAT32 文件系统实现（对接新版 VFS inode 接口）
 *
 * 引用约定（与 vfs.h 生命周期文档一致）：
 *   - lookup/create/mkdir 返回的 inode refcount = 1，引用归 caller 所有，
 *     VFS 层会在 cache miss 分支把它当作 lookup 临时引用接管。
 *   - sb->root 的 refcount = 1，引用归 super_block 所有，kill_sb 时释放。
 *   - destroy 负责 free private 与 inode 本身。
 *
 * 限制：
 *   - 只支持 8.3 大写文件名（长文件名 LFN 目录项会被跳过）。
 *   - 目录不落 "." / ".." 目录项，lookup(".""..") 返回失败，
 *     内核侧路径规范化（do_build_user_path）已在文本层处理 ".."。
 *   - 元数据操作不做并发互斥（沿用原 fat12 的单写者假设）。
 */

/*
 * 磁盘结构
 * */

struct fat32_bpb {
	uint8_t bs_jmp_boot[3];
	uint8_t bs_oem_name[8];

	uint16_t bpb_bytes_per_sector;
	uint8_t bpb_sectors_per_cluster;
	uint16_t bpb_reserved_sectors;
	uint8_t bpb_num_fats;
	uint16_t bpb_root_entries;
	uint16_t bpb_total_sectors;
	uint8_t bpb_media_descriptor;
	uint16_t bpb_sectors_per_fat;
	uint16_t bpb_sectors_per_track;
	uint16_t bpb_num_heads;
	uint32_t bpb_hidden_sectors;
	uint32_t bpb_total_sectors_large;

	/* FAT32 扩展 */
	uint32_t bpb_sectors_per_fat32;
	uint16_t bpb_ext_flags;
	uint16_t bpb_fs_version;
	uint32_t bpb_root_cluster;
	uint16_t bpb_fsinfo_sector;
	uint16_t bpb_backup_boot_sector;
	uint8_t bs_reserved[12];
	uint8_t bs_drv_num;
	uint8_t bs_reserved1;
	uint8_t bs_boot_sig;
	uint32_t bs_vol_id;
	uint8_t bs_vol_label[11];
	uint8_t bs_file_sys_type[8];

	uint8_t signature[2];
} __attribute__((packed));

struct fat32_dirent32 {
	uint8_t dir_name[11];
	uint8_t dir_attr;
	uint8_t dir_nt_reserved;
	uint8_t dir_create_time_tenth;
	uint16_t dir_create_time;
	uint16_t dir_create_date;
	uint16_t dir_last_access_date;
	uint16_t dir_first_cluster_high;
	uint16_t dir_write_time;
	uint16_t dir_write_date;
	uint16_t dir_first_cluster_low;
	uint32_t dir_file_size;
} __attribute__((packed));

/* FAT 时间戳：mkfs/创建时写入，getattr 时读回换算成 unix 秒 */
struct fat32_fat_time {
	uint16_t date;
	uint16_t time;
};

/*
 * 小工具
 * */

static inline uint32_t fat32_cluster_bytes(struct fat32_fs_priv* fs)
{
	return fs->bytes_per_sector * fs->sectors_per_cluster;
}

static inline uint32_t fat32_cluster_to_sector(struct fat32_fs_priv* fs,
					       uint32_t cluster)
{
	return fs->data_start + (cluster - 2) * fs->sectors_per_cluster;
}

static inline int fat32_is_valid_cluster(struct fat32_fs_priv* fs,
					 uint32_t cluster)
{
	return cluster >= 2 && cluster < fs->cluster_count + 2 &&
	       cluster < FAT32_EOF_MASK;
}

static inline int fat32_is_chain_end(uint32_t v)
{
	return v >= FAT32_EOF_MASK || v == FAT32_BAD_MARK || v == 0;
}

static int
fat32_read_sector(struct fat32_fs_priv* fs, uint64_t sector, void* buf)
{
	return fs->bdev->driver.read(fs->bdev->private_data, sector, buf);
}

static int
fat32_write_sector(struct fat32_fs_priv* fs, uint64_t sector, const void* buf)
{
	return fs->bdev->driver.write(fs->bdev->private_data, sector, buf);
}

/* 读写 FAT 表项（低28位有效） */
static int
fat32_fat_get(struct fat32_fs_priv* fs, uint32_t cluster, uint32_t* out)
{
	uint32_t sec;
	uint32_t off;
	uint8_t* buf;
	uint32_t v;
	int ret;

	if (out == NULL || cluster < 2 || cluster >= fs->cluster_count + 2)
		return -1;

	sec = fs->fat_start + (cluster * 4) / fs->bytes_per_sector;
	off = (cluster * 4) % fs->bytes_per_sector;

	buf = slab_alloc(fs->bytes_per_sector);
	if (buf == NULL)
		return -1;

	ret = fat32_read_sector(fs, sec, buf);

	if (ret < 0) {
		slab_free(buf);
		return -1;
	}

	memcpy(&v, buf + off, sizeof(v));

	slab_free(buf);

	*out = v & 0x0FFFFFFF;

	return 0;
}

static int
fat32_fat_set(struct fat32_fs_priv* fs, uint32_t cluster, uint32_t value)
{
	uint32_t sec;
	uint32_t off;
	uint8_t* buf;
	uint32_t v;
	int ret;

	if (cluster < 2 || cluster >= fs->cluster_count + 2)
		return -1;

	sec = fs->fat_start + (cluster * 4) / fs->bytes_per_sector;
	off = (cluster * 4) % fs->bytes_per_sector;

	buf = slab_alloc(fs->bytes_per_sector);
	if (buf == NULL)
		return -1;

	ret = fat32_read_sector(fs, sec, buf);

	if (ret < 0) {
		slab_free(buf);
		return -1;
	}

	memcpy(&v, buf + off, sizeof(v));
	v = (v & 0xF0000000) | (value & 0x0FFFFFFF);
	memcpy(buf + off, &v, sizeof(v));

	ret = fat32_write_sector(fs, sec, buf);

	slab_free(buf);

	return ret;
}

/* 分配一个空闲簇：标记 EOF 并清零数据区 */
static int fat32_alloc_cluster(struct fat32_fs_priv* fs, uint32_t* out)
{
	uint8_t* zero;
	uint32_t c;

	zero = slab_alloc(fs->bytes_per_sector);

	if (zero == NULL)
		return -1;

	memset(zero, 0, fs->bytes_per_sector);

	for (c = 2; c < fs->cluster_count + 2; c++) {
		uint32_t v;

		if (fat32_fat_get(fs, c, &v) < 0)
			goto fail;

		if (v != 0)
			continue;

		if (fat32_fat_set(fs, c, FAT32_EOF_MARK) < 0)
			goto fail;

		for (uint32_t i = 0; i < fs->sectors_per_cluster; i++) {
			if (fat32_write_sector(
				fs, fat32_cluster_to_sector(fs, c) + i, zero) <
			    0) {
				fat32_fat_set(fs, c, FAT32_FREE_MARK);
				goto fail;
			}
		}

		slab_free(zero);
		*out = c;
		return 0;
	}

fail:
	slab_free(zero);
	return -1;
}

/* 释放从 start 开始的整条簇链 */
static int fat32_free_chain(struct fat32_fs_priv* fs, uint32_t start)
{
	uint32_t c = start;
	uint32_t guard = 0;

	while (fat32_is_valid_cluster(fs, c) && guard++ < fs->cluster_count) {
		uint32_t next;

		if (fat32_fat_get(fs, c, &next) < 0)
			return -1;

		if (fat32_fat_set(fs, c, FAT32_FREE_MARK) < 0)
			return -1;

		if (fat32_is_chain_end(next))
			break;

		c = next;
	}

	return 0;
}

/* 统计簇链长度 */
static uint32_t fat32_chain_len(struct fat32_fs_priv* fs, uint32_t start)
{
	uint32_t c = start;
	uint32_t n = 0;

	while (fat32_is_valid_cluster(fs, c) && n < fs->cluster_count) {
		uint32_t next;

		if (fat32_fat_get(fs, c, &next) < 0)
			break;

		n++;

		if (fat32_is_chain_end(next))
			break;

		c = next;
	}

	return n;
}

/* 追加 n 个簇到链尾（*tail 为当前尾簇，0 表示空链），返回 0 成功 */
static int fat32_append_clusters(struct fat32_fs_priv* fs,
				 uint32_t* first,
				 uint32_t* tail,
				 uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++) {
		uint32_t c;

		if (fat32_alloc_cluster(fs, &c) < 0)
			return -1;

		if (*tail == 0)
			*first = c;
		else
			fat32_fat_set(fs, *tail, c);

		*tail = c;
	}

	return 0;
}

/* 时间戳：FAT 时间 <-> unix 秒（本地近似，无时区） */
static void fat32_unix_to_fat(uint32_t ux_sec, struct fat32_fat_time* out)
{
	uint32_t sec = ux_sec % 60;
	uint32_t min;
	uint32_t hour;
	uint32_t days;
	uint16_t year = 1970;
	uint8_t month = 1;
	static const uint8_t days_in_month[] = {
	    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

	ux_sec /= 60;
	min = ux_sec % 60;
	ux_sec /= 60;
	hour = ux_sec % 24;
	days = ux_sec / 24;

	while (1) {
		uint16_t ydays = 365;

		if ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)
			ydays = 366;

		if (days >= ydays) {
			days -= ydays;
			year++;
		} else
			break;
	}

	for (int m = 0; m < 12; m++) {
		uint8_t md = days_in_month[m];

		if (m == 1 &&
		    ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
			md = 29;

		if (days >= md) {
			days -= md;
			month++;
		} else
			break;
	}

	if (year < 1980) {
		year = 1980;
		month = 1;
		days = 0;
	}

	out->time = (hour << 11) | (min << 5) | (sec / 2);
	out->date = ((year - 1980) << 9) | (month << 5) | (days + 1);
}

static uint64_t fat32_fat_to_unix(struct fat32_fat_time t)
{
	uint32_t year = 1980 + ((t.date >> 9) & 0x7F);
	uint32_t month = (t.date >> 5) & 0xF;
	uint32_t day = t.date & 0x1F;
	uint32_t hour = (t.time >> 11) & 0x1F;
	uint32_t min = (t.time >> 5) & 0x3F;
	uint32_t sec = (t.time & 0x1F) * 2;
	uint64_t days;
	uint32_t y, era, yoe, doy, doe;

	if (month < 1)
		month = 1;
	if (month > 12)
		month = 12;
	if (day < 1)
		day = 1;
	if (day > 31)
		day = 31;

	y = year - (month <= 2);
	era = y / 400;
	yoe = y - era * 400;
	doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	days = (uint64_t)era * 146097 + doe - 719468;

	return days * 86400 + hour * 3600 + min * 60 + sec;
}

static void fat32_now(struct fat32_fat_time* out)
{
	uint64_t ticks = get_sys_timer_tick();
	uint32_t ux_sec = (uint32_t)(ticks / 100) + 1700000000U;

	fat32_unix_to_fat(ux_sec, out);
}

/* ============================================================
 * 目录项定位
 * ============================================================ */

/* 读回 inode 对应的磁盘目录项 */
static int fat32_dirent_load(struct fat32_fs_priv* fs,
			     struct fat32_inode_priv* priv,
			     struct fat32_dirent32* out)
{
	uint32_t sec;
	uint32_t off;
	uint8_t* buf;
	int ret;

	if (priv->is_root)
		return -1;

	sec = fat32_cluster_to_sector(fs, priv->dir_cluster) +
	      priv->dirent_off / fs->bytes_per_sector;
	off = priv->dirent_off % fs->bytes_per_sector;

	buf = slab_alloc(fs->bytes_per_sector);

	if (buf == NULL)
		return -1;

	ret = fat32_read_sector(fs, sec, buf);

	if (ret < 0) {
		slab_free(buf);
		return -1;
	}

	memcpy(out, buf + off, sizeof(*out));

	slab_free(buf);

	return 0;
}

/* 回写磁盘目录项（调用者负责填好字段） */
static int fat32_dirent_store(struct fat32_fs_priv* fs,
			      struct fat32_inode_priv* priv,
			      const struct fat32_dirent32* e)
{
	uint32_t sec;
	uint32_t off;
	uint8_t* buf;
	int ret;

	if (priv->is_root)
		return -1;

	sec = fat32_cluster_to_sector(fs, priv->dir_cluster) +
	      priv->dirent_off / fs->bytes_per_sector;
	off = priv->dirent_off % fs->bytes_per_sector;

	buf = slab_alloc(fs->bytes_per_sector);

	if (buf == NULL)
		return -1;

	ret = fat32_read_sector(fs, sec, buf);

	if (ret < 0) {
		slab_free(buf);
		return -1;
	}

	memcpy(buf + off, e, sizeof(*e));

	ret = fat32_write_sector(fs, sec, buf);

	slab_free(buf);

	return ret;
}

/* 更新文件大小与修改时间到磁盘目录项 */
static int fat32_dirent_update_size(struct fat32_fs_priv* fs,
				    struct fat32_inode_priv* priv,
				    uint32_t size)
{
	struct fat32_dirent32 e;
	struct fat32_fat_time now;

	if (priv->is_root)
		return 0;

	if (fat32_dirent_load(fs, priv, &e) < 0)
		return -1;

	e.dir_file_size = size;
	fat32_now(&now);
	e.dir_write_time = now.time;
	e.dir_write_date = now.date;

	return fat32_dirent_store(fs, priv, &e);
}

/* ============================================================
 * 名字转换（8.3）
 * ============================================================ */

static inline int fat32_char_ok(char c)
{
	unsigned char u = (unsigned char)c;

	if (u >= 'A' && u <= 'Z')
		return 1;
	if (u >= 'a' && u <= 'z')
		return 1;
	if (u >= '0' && u <= '9')
		return 1;
	if (u >= 0x80)
		return 1;

	return strchr("!#$%&'()-@^_`{}~", u) != NULL;
}

/*
 * "name.ext" -> 11 字节 8.3 大写形式
 * 名字非法返回 -1（太长 / 多个点 / 非法字符 / 以点开头）
 */
static int fat32_name_to_dos(const char* name, uint8_t out[11])
{
	size_t len = strlen(name);
	const char* dot = NULL;
	size_t base_len;
	size_t ext_len = 0;
	size_t i;

	if (name == NULL || len == 0 || len > 12)
		return -1;

	if (name[0] == '.')
		return -1;

	for (i = 0; i < len; i++) {
		if (name[i] == '.') {
			if (dot != NULL)
				return -1;
			dot = &name[i];
		} else if (!fat32_char_ok(name[i])) {
			return -1;
		}
	}

	base_len = dot ? (size_t)(dot - name) : len;

	if (dot != NULL)
		ext_len = len - base_len - 1;

	if (base_len == 0 || base_len > 8)
		return -1;
	if (dot != NULL && ext_len > 3)
		return -1;

	memset(out, ' ', 11);

	for (i = 0; i < base_len; i++) {
		char c = name[i];

		out[i] = (c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c;
	}

	if (dot != NULL) {
		for (i = 0; i < ext_len; i++) {
			char c = dot[1 + i];

			out[8 + i] =
			    (c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c;
		}
	}

	return 0;
}

/* 11 字节 8.3 -> "name.ext"（按 NT 大小写标志还原小写） */
static void fat32_dos_to_name(const uint8_t raw[11], uint8_t ntres, char* out)
{
	int base_len = 0;
	int ext_len = 0;
	int i;
	int p = 0;

	for (i = 0; i < 8; i++) {
		if (raw[i] == ' ')
			break;
		base_len++;
	}

	for (i = 0; i < 3; i++) {
		if (raw[8 + i] == ' ')
			break;
		ext_len++;
	}

	for (i = 0; i < base_len; i++) {
		char c = raw[i];

		if ((ntres & 0x08) && c >= 'A' && c <= 'Z')
			c = c - 'A' + 'a';
		out[p++] = c;
	}

	if (ext_len > 0) {
		out[p++] = '.';

		for (i = 0; i < ext_len; i++) {
			char c = raw[8 + i];

			if ((ntres & 0x10) && c >= 'A' && c <= 'Z')
				c = c - 'A' + 'a';
			out[p++] = c;
		}
	}

	out[p] = '\0';
}

/* 目录项是否为 8.3 基本项（非 LFN / 卷标 / 删除项） */
static inline int fat32_dirent_basic(const struct fat32_dirent32* e)
{
	if (e->dir_name[0] == 0x00 || e->dir_name[0] == 0xE5)
		return 0;

	if (e->dir_attr == FAT32_ATTR_LONG_NAME)
		return 0;

	if ((e->dir_attr & FAT32_ATTR_VOLUME_ID) &&
	    !(e->dir_attr & FAT32_ATTR_DIRECTORY))
		return 0;

	return 1;
}

/* "." / ".." 目录项（防御性跳过，本实现不生成） */
static inline int fat32_dirent_dot(const struct fat32_dirent32* e)
{
	return e->dir_name[0] == '.';
}

/* ============================================================
 * 目录遍历
 * ============================================================ */

/*
 * 遍历目录簇链上的所有原始目录项。
 *
 * cb 返回 0    : 继续遍历
 * cb 返回 1    : 停止，fat32_dir_walk 返回 1（由调用者解释为"找到"）
 * cb 返回 -1   : 出错停止
 *
 * 簇链结束返回 0。
 * 坐标 (cluster, off) 是目录项在目录数据中的绝对位置。
 *
 * 注意：这里不在 0x00（目录结束标记）处提前返回——
 * 各回调通过 fat32_dirent_basic() 自行过滤 0x00 / 0xE5 项，
 * 而 cb_free_slot 恰恰需要看到 0x00 槽位才能分配新目录项。
 */
typedef int (*fat32_dir_cb)(struct fat32_dirent32* e,
			    uint32_t cluster,
			    uint32_t off,
			    void* arg);

static int fat32_dir_walk(struct inode* dir, fat32_dir_cb cb, void* arg)
{
	struct fat32_fs_priv* fs = dir->sb->private;
	struct fat32_inode_priv* dp = dir->private;
	uint8_t* buf;
	uint32_t cluster;
	uint32_t guard = 0;
	int ret = 0;

	buf = slab_alloc(fs->bytes_per_sector);

	if (buf == NULL)
		return -1;

	cluster = dp->is_root ? fs->root_cluster : dp->first_cluster;

	while (fat32_is_valid_cluster(fs, cluster) &&
	       guard++ < fs->cluster_count) {
		for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
			if (fat32_read_sector(
				fs,
				fat32_cluster_to_sector(fs, cluster) + s,
				buf) < 0) {
				slab_free(buf);
				return -1;
			}

			for (uint32_t i = 0;
			     i < fs->bytes_per_sector / FAT32_DIRENT_SIZE;
			     i++) {
				struct fat32_dirent32* e =
				    (struct
				     fat32_dirent32*)(buf +
						      i * FAT32_DIRENT_SIZE);
				uint32_t off = s * fs->bytes_per_sector +
					       i * FAT32_DIRENT_SIZE;
				int r;

				/*
				 * 0x00（目录结束标记）与 0xE5（删除项）不在此
				 * 提前终止——交给回调处理：find/readdir/empty
				 * 用 fat32_dirent_basic() 过滤，cb_free_slot
				 * 则需要把 0x00 作为空闲槽位候选。
				 */
				r = cb(e, cluster, off, arg);

				if (r != 0) {
					slab_free(buf);
					return r;
				}
			}
		}

		if (fat32_fat_get(fs, cluster, &cluster) < 0) {
			slab_free(buf);
			return -1;
		}
	}

	slab_free(buf);

	return ret;
}

struct fat32_find_arg {
	uint8_t dos[11];
	struct fat32_dirent32 e;
	uint32_t dir_cluster;
	uint32_t dirent_off;
	int found;
};

static int fat32_cb_find(struct fat32_dirent32* e,
			 uint32_t cluster,
			 uint32_t off,
			 void* arg)
{
	struct fat32_find_arg* a = arg;

	if (!fat32_dirent_basic(e) || fat32_dirent_dot(e))
		return 0;

	if (memcmp(e->dir_name, a->dos, 11) == 0) {
		a->found = 1;
		a->e = *e;
		a->dir_cluster = cluster;
		a->dirent_off = off;
		return 1;
	}

	return 0;
}

/* 在 dir 中查找 name（8.3 大写形式比较），找到填 arg */
static int fat32_dir_find(struct inode* dir,
			  const uint8_t dos[11],
			  struct fat32_find_arg* arg)
{
	memset(arg, 0, sizeof(*arg));
	memcpy(arg->dos, dos, 11);

	return fat32_dir_walk(dir, fat32_cb_find, arg) == 1 ? 0 : -1;
}

struct fat32_slot_arg {
	uint32_t dir_cluster;
	uint32_t dirent_off;
	int found;
};

/*
 * 找一个空闲目录项槽位（0x00 或 0xE5）。
 * fat32_dir_walk 不会在 0x00 处提前终止，因此这里能看到
 * 目录尾部的空闲区域；找到第一个空闲槽位即停。
 */
static int fat32_cb_free_slot(struct fat32_dirent32* e,
			      uint32_t cluster,
			      uint32_t off,
			      void* arg)
{
	struct fat32_slot_arg* a = arg;

	if (e->dir_name[0] == 0x00 || e->dir_name[0] == 0xE5) {
		a->found = 1;
		a->dir_cluster = cluster;
		a->dirent_off = off;
		return 1;
	}

	return 0;
}

/* 扩展目录簇链一个簇并清零 */
static int fat32_dir_extend(struct inode* dir, uint32_t* new_cluster)
{
	struct fat32_fs_priv* fs = dir->sb->private;
	struct fat32_inode_priv* dp = dir->private;
	uint32_t first = dp->is_root ? fs->root_cluster : dp->first_cluster;
	uint32_t tail;
	uint32_t guard = 0;

	tail = first;

	while (fat32_is_valid_cluster(fs, tail) &&
	       guard++ < fs->cluster_count) {
		uint32_t next;

		if (fat32_fat_get(fs, tail, &next) < 0)
			return -1;

		if (fat32_is_chain_end(next))
			break;

		tail = next;
	}

	if (!fat32_is_valid_cluster(fs, tail))
		return -1;

	if (fat32_alloc_cluster(fs, new_cluster) < 0)
		return -1;

	if (fat32_fat_set(fs, tail, *new_cluster) < 0) {
		fat32_free_chain(fs, *new_cluster);
		return -1;
	}

	return 0;
}

/* ============================================================
 * inode 构造
 * ============================================================ */

static struct inode_operations fat32_inode_ops;
static struct file_operations fat32_file_ops;

static void fat32_inode_destroy(struct inode* inode);
static int fat32_truncate(struct inode* inode, uint64_t size);

/*
 * 由目录项构造一个 VFS inode。
 * refcount = 1，引用归 caller 所有。
 */
static struct inode* fat32_inode_create(struct super_block* sb,
					const struct fat32_dirent32* e,
					uint32_t dir_cluster,
					uint32_t dirent_off)
{
	struct fat32_fs_priv* fs = sb->private;
	struct inode* inode;
	struct fat32_inode_priv* priv;
	uint32_t first;

	inode = slab_alloc(sizeof(*inode));
	priv = slab_alloc(sizeof(*priv));

	if (inode == NULL || priv == NULL) {
		if (inode != NULL)
			slab_free(inode);
		if (priv != NULL)
			slab_free(priv);
		return NULL;
	}

	memset(inode, 0, sizeof(*inode));
	memset(priv, 0, sizeof(*priv));

	first = ((uint32_t)e->dir_first_cluster_high << 16) |
		e->dir_first_cluster_low;

	priv->first_cluster = first;
	priv->attr = e->dir_attr;
	priv->dir_cluster = dir_cluster;
	priv->dirent_off = dirent_off;
	priv->is_root = 0;

	inode->sb = sb;
	inode->ino = dir_cluster * fs->bytes_per_sector + dirent_off;

	if (e->dir_attr & FAT32_ATTR_DIRECTORY) {
		inode->mode = S_IFDIR | 0755;
		/* 目录大小 = 簇链总字节数（readdir 不依赖它，仅展示用） */
		inode->size =
		    fat32_chain_len(fs, first) * fat32_cluster_bytes(fs);
	} else {
		uint32_t perm = 0644;

		if (e->dir_attr & FAT32_ATTR_READ_ONLY)
			perm = 0444;

		inode->mode = S_IFREG | perm;
		inode->size = e->dir_file_size;
	}

	inode->uid = 0;
	inode->gid = 0;
	inode->iops = &fat32_inode_ops;
	inode->fops = &fat32_file_ops;
	inode->private = priv;
	inode->refcount = 1;

	init_spinlock(&inode->lock);

	return inode;
}

/* 构造根目录 inode，refcount = 1，引用归 super_block 所有 */
static struct inode* fat32_root_inode(struct super_block* sb)
{
	struct fat32_fs_priv* fs = sb->private;
	struct inode* inode;
	struct fat32_inode_priv* priv;

	inode = slab_alloc(sizeof(*inode));
	priv = slab_alloc(sizeof(*priv));

	if (inode == NULL || priv == NULL) {
		if (inode != NULL)
			slab_free(inode);
		if (priv != NULL)
			slab_free(priv);
		return NULL;
	}

	memset(inode, 0, sizeof(*inode));
	memset(priv, 0, sizeof(*priv));

	priv->first_cluster = fs->root_cluster;
	priv->is_root = 1;

	inode->sb = sb;
	inode->ino = fs->root_cluster;
	inode->mode = S_IFDIR | 0755;
	inode->size =
	    fat32_chain_len(fs, fs->root_cluster) * fat32_cluster_bytes(fs);
	inode->iops = &fat32_inode_ops;
	inode->fops = &fat32_file_ops;
	inode->private = priv;
	inode->refcount = 1;

	init_spinlock(&inode->lock);

	return inode;
}

static void fat32_inode_destroy(struct inode* inode)
{
	if (inode->private != NULL)
		slab_free(inode->private);

	slab_free(inode);
}

/* ============================================================
 * inode_operations
 * ============================================================ */

static int fat32_lookup(struct inode* dir, const char* name, struct inode** out)
{
	struct fat32_find_arg found;
	uint8_t dos[11];
	struct inode* ni;

	if (dir == NULL || name == NULL || out == NULL || dir->sb == NULL ||
	    dir->sb->private == NULL || dir->private == NULL)
		return -1;

	if ((dir->mode & S_IFMT) != S_IFDIR)
		return -1;

	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
		return -1;

	if (fat32_name_to_dos(name, dos) < 0)
		return -1;

	if (fat32_dir_find(dir, dos, &found) < 0)
		return -1;

	ni = fat32_inode_create(
	    dir->sb, &found.e, found.dir_cluster, found.dirent_off);

	if (ni == NULL)
		return -1;

	/* 引用归 caller */
	*out = ni;

	return 0;
}

/*
 * 在 dir 中新建目录项并返回新 inode。
 * file: 簇号 0（惰性分配），size 0。
 * dir : 立即分配一个簇并清零。
 */
static int fat32_dir_add(struct inode* dir,
			 const char* name,
			 uint32_t mode,
			 struct inode** out)
{
	struct fat32_fs_priv* fs;
	struct fat32_slot_arg slot;
	struct fat32_dirent32 e;
	struct fat32_fat_time now;
	struct inode* ni;
	uint8_t dos[11];
	uint8_t ntres = 0;
	uint32_t cluster = 0;
	int is_dir = (mode & S_IFMT) == S_IFDIR;
	const char* dot;

	if (dir == NULL || name == NULL || out == NULL || dir->sb == NULL ||
	    dir->sb->private == NULL || dir->private == NULL)
		return -1;

	fs = dir->sb->private;

	if ((dir->mode & S_IFMT) != S_IFDIR)
		return -1;

	if (fat32_name_to_dos(name, dos) < 0)
		return -1;

	/* 已存在则失败 */
	{
		struct fat32_find_arg found;

		if (fat32_dir_find(dir, dos, &found) == 0)
			return -1;
	}

	/* 文件惰性分配簇；目录立即分配 */
	if (is_dir) {
		if (fat32_alloc_cluster(fs, &cluster) < 0)
			return -1;
	}

	/* 找空闲槽位，找不到则扩展目录 */
	memset(&slot, 0, sizeof(slot));

	if (fat32_dir_walk(dir, fat32_cb_free_slot, &slot) == 1) {
		/* slot 已填好 */
	} else {
		uint32_t nc;

		if (fat32_dir_extend(dir, &nc) < 0) {
			if (is_dir)
				fat32_free_chain(fs, cluster);
			return -1;
		}

		slot.found = 1;
		slot.dir_cluster = nc;
		slot.dirent_off = 0;
	}

	memset(&e, 0, sizeof(e));
	memcpy(e.dir_name, dos, 11);

	e.dir_attr = is_dir ? FAT32_ATTR_DIRECTORY : FAT32_ATTR_ARCHIVE;

	/* NT 大小写标志：还原小写显示 */
	dot = strchr(name, '.');

	for (const char* p = name; dot != NULL && p < dot; p++)
		if (*p >= 'a' && *p <= 'z')
			ntres |= 0x08;

	if (dot != NULL) {
		for (const char* p = dot + 1; *p; p++)
			if (*p >= 'a' && *p <= 'z')
				ntres |= 0x10;
	}

	e.dir_nt_reserved = ntres;

	fat32_now(&now);
	e.dir_create_time_tenth = 0;
	e.dir_create_time = now.time;
	e.dir_create_date = now.date;
	e.dir_last_access_date = now.date;
	e.dir_write_time = now.time;
	e.dir_write_date = now.date;
	e.dir_first_cluster_high = cluster >> 16;
	e.dir_first_cluster_low = cluster & 0xFFFF;
	e.dir_file_size = 0;

	{
		uint32_t sec = fat32_cluster_to_sector(fs, slot.dir_cluster) +
			       slot.dirent_off / fs->bytes_per_sector;
		uint32_t off = slot.dirent_off % fs->bytes_per_sector;
		uint8_t* buf = slab_alloc(fs->bytes_per_sector);

		if (buf == NULL) {
			if (is_dir)
				fat32_free_chain(fs, cluster);
			return -1;
		}

		if (fat32_read_sector(fs, sec, buf) < 0) {
			slab_free(buf);
			if (is_dir)
				fat32_free_chain(fs, cluster);
			return -1;
		}

		memcpy(buf + off, &e, sizeof(e));

		if (fat32_write_sector(fs, sec, buf) < 0) {
			slab_free(buf);
			if (is_dir)
				fat32_free_chain(fs, cluster);
			return -1;
		}

		slab_free(buf);
	}

	ni = fat32_inode_create(dir->sb, &e, slot.dir_cluster, slot.dirent_off);

	if (ni == NULL) {
		if (is_dir)
			fat32_free_chain(fs, cluster);
		return -1;
	}

	/* 引用归 caller */
	*out = ni;

	return 0;
}

static int fat32_create(struct inode* dir,
			const char* name,
			uint32_t mode,
			struct inode** inode)
{
	if ((mode & S_IFMT) != S_IFREG)
		return -1;

	return fat32_dir_add(dir, name, mode, inode);
}

static int fat32_mkdir(struct inode* dir,
		       const char* name,
		       uint32_t mode,
		       struct inode** inode)
{
	if ((mode & S_IFMT) != S_IFDIR)
		return -1;

	return fat32_dir_add(dir, name, mode, inode);
}

static int fat32_unlink(struct inode* dir, const char* name)
{
	struct fat32_fs_priv* fs;
	struct fat32_find_arg found;
	struct fat32_dirent32 e;
	uint8_t dos[11];
	uint32_t first;

	if (dir == NULL || name == NULL || dir->sb == NULL ||
	    dir->sb->private == NULL || dir->private == NULL)
		return -1;

	fs = dir->sb->private;

	if ((dir->mode & S_IFMT) != S_IFDIR)
		return -1;

	if (fat32_name_to_dos(name, dos) < 0)
		return -1;

	if (fat32_dir_find(dir, dos, &found) < 0)
		return -1;

	if (found.e.dir_attr & FAT32_ATTR_DIRECTORY)
		return -1;

	first = ((uint32_t)found.e.dir_first_cluster_high << 16) |
		found.e.dir_first_cluster_low;

	if (fat32_free_chain(fs, first) < 0)
		return -1;

	e = found.e;
	e.dir_name[0] = 0xE5;
	e.dir_first_cluster_high = 0;
	e.dir_first_cluster_low = 0;
	e.dir_file_size = 0;

	{
		struct fat32_inode_priv tmp = {0};

		tmp.dir_cluster = found.dir_cluster;
		tmp.dirent_off = found.dirent_off;
		tmp.is_root = 0;

		if (fat32_dirent_store(fs, &tmp, &e) < 0)
			return -1;
	}

	return 0;
}

struct fat32_empty_arg {
	int has_entries;
};

static int fat32_cb_empty(struct fat32_dirent32* e,
			  uint32_t cluster,
			  uint32_t off,
			  void* arg)
{
	struct fat32_empty_arg* a = arg;

	(void)cluster;
	(void)off;

	if (!fat32_dirent_basic(e))
		return 0;

	if (fat32_dirent_dot(e))
		return 0;

	a->has_entries = 1;

	return 1;
}

static int fat32_rmdir(struct inode* dir, const char* name)
{
	struct fat32_fs_priv* fs;
	struct fat32_find_arg found;
	struct fat32_dirent32 e;
	struct fat32_empty_arg empty;
	uint8_t dos[11];
	uint32_t first;
	struct inode tmp_dir;

	if (dir == NULL || name == NULL || dir->sb == NULL ||
	    dir->sb->private == NULL || dir->private == NULL)
		return -1;

	fs = dir->sb->private;

	if ((dir->mode & S_IFMT) != S_IFDIR)
		return -1;

	if (fat32_name_to_dos(name, dos) < 0)
		return -1;

	if (fat32_dir_find(dir, dos, &found) < 0)
		return -1;

	if (!(found.e.dir_attr & FAT32_ATTR_DIRECTORY))
		return -1;

	first = ((uint32_t)found.e.dir_first_cluster_high << 16) |
		found.e.dir_first_cluster_low;

	if (first == 0)
		return -1;

	/*
	 * 检查目标目录是否为空：
	 * 复用一个临时 inode 走 fat32_dir_walk。
	 */
	memset(&tmp_dir, 0, sizeof(tmp_dir));
	tmp_dir.sb = dir->sb;
	tmp_dir.mode = S_IFDIR;
	tmp_dir.private = &(struct fat32_inode_priv){
	    .first_cluster = first,
	    .is_root = 0,
	};

	empty.has_entries = 0;

	if (fat32_dir_walk(&tmp_dir, fat32_cb_empty, &empty) < 0)
		return -1;

	if (empty.has_entries)
		return -1;

	if (fat32_free_chain(fs, first) < 0)
		return -1;

	e = found.e;
	e.dir_name[0] = 0xE5;
	e.dir_first_cluster_high = 0;
	e.dir_first_cluster_low = 0;
	e.dir_file_size = 0;

	{
		struct fat32_inode_priv tmp = {0};

		tmp.dir_cluster = found.dir_cluster;
		tmp.dirent_off = found.dirent_off;
		tmp.is_root = 0;

		if (fat32_dirent_store(fs, &tmp, &e) < 0)
			return -1;
	}

	return 0;
}

struct fat32_readdir_arg {
	struct fat32_fs_priv* fs;
	uint64_t* offset;
	struct vfs_dirent* out;
	uint64_t pos; /* 当前原始槽位字节坐标 */
	int emitted;
};

static int fat32_cb_readdir(struct fat32_dirent32* e,
			    uint32_t cluster,
			    uint32_t off,
			    void* arg)
{
	struct fat32_readdir_arg* a = arg;

	(void)cluster;

	if (a->emitted)
		return 1;

	/* 只统计 basic 项：cookie 记录"下一个要看的原始槽位" */
	if (a->pos < *a->offset) {
		a->pos += FAT32_DIRENT_SIZE;
		return 0;
	}

	if (!fat32_dirent_basic(e) || fat32_dirent_dot(e)) {
		a->pos += FAT32_DIRENT_SIZE;
		*a->offset = a->pos;
		return 0;
	}

	fat32_dos_to_name(e->dir_name, e->dir_nt_reserved, a->out->name);

	a->out->ino = ((uint64_t)cluster * 512 + off);
	a->out->type = (e->dir_attr & FAT32_ATTR_DIRECTORY) ? S_IFDIR : S_IFREG;

	a->pos += FAT32_DIRENT_SIZE;
	*a->offset = a->pos;
	a->emitted = 1;

	return 1;
}

static int
fat32_readdir(struct inode* dir, uint64_t* offset, struct vfs_dirent* dirent)
{
	struct fat32_readdir_arg arg;

	if (dir == NULL || offset == NULL || dirent == NULL ||
	    dir->sb == NULL || dir->sb->private == NULL || dir->private == NULL)
		return -1;

	if ((dir->mode & S_IFMT) != S_IFDIR)
		return -1;

	memset(&arg, 0, sizeof(arg));
	arg.fs = dir->sb->private;
	arg.offset = offset;
	arg.out = dirent;

	/* cookie 按 32 对齐 */
	*offset = *offset / FAT32_DIRENT_SIZE * FAT32_DIRENT_SIZE;

	fat32_dir_walk(dir, fat32_cb_readdir, &arg);

	return arg.emitted ? 0 : -1;
}

static int fat32_getattr(struct inode* inode, struct vfs_kstat* stat)
{
	struct fat32_fs_priv* fs;
	struct fat32_inode_priv* priv;
	struct fat32_dirent32 e;

	if (inode == NULL || stat == NULL || inode->sb == NULL ||
	    inode->sb->private == NULL || inode->private == NULL)
		return -1;

	fs = inode->sb->private;
	priv = inode->private;

	memset(stat, 0, sizeof(*stat));

	stat->ino = inode->ino;
	stat->mode = inode->mode;
	stat->uid = inode->uid;
	stat->gid = inode->gid;
	stat->size = inode->size;
	stat->nlink = 1;
	stat->blocks =
	    (inode->size + fs->bytes_per_sector - 1) / fs->bytes_per_sector;
	stat->blksize = fs->bytes_per_sector;

	if (!priv->is_root && fat32_dirent_load(fs, priv, &e) == 0) {
		struct fat32_fat_time c = {.date = e.dir_create_date,
					   .time = e.dir_create_time};
		struct fat32_fat_time w = {.date = e.dir_write_date,
					   .time = e.dir_write_time};
		struct fat32_fat_time a = {.date = e.dir_last_access_date,
					   .time = 0};

		stat->ctime = fat32_fat_to_unix(c);
		stat->mtime = fat32_fat_to_unix(w);
		stat->atime = fat32_fat_to_unix(a);
	}

	return 0;
}

/* ============================================================
 * 数据读写
 * ============================================================ */

static int fat32_read_data(struct inode* inode,
			   uint64_t pos,
			   void* buf,
			   uint32_t count,
			   uint32_t* out_len)
{
	struct fat32_fs_priv* fs = inode->sb->private;
	struct fat32_inode_priv* priv = inode->private;
	uint32_t cluster_bytes = fat32_cluster_bytes(fs);
	uint8_t* scratch;
	uint8_t* out = buf;
	uint32_t cluster;
	uint32_t coff;
	uint64_t remaining;
	uint64_t avail;

	if (pos >= inode->size) {
		*out_len = 0;
		return 0;
	}

	avail = inode->size - pos;

	if (count > avail)
		count = avail;

	scratch = slab_alloc(fs->bytes_per_sector);

	if (scratch == NULL)
		return -1;

	cluster = priv->first_cluster;
	coff = pos % cluster_bytes;
	remaining = count;

	/* 跳到起始簇 */
	for (uint64_t i = 0; i < pos / cluster_bytes; i++) {
		if (!fat32_is_valid_cluster(fs, cluster)) {
			slab_free(scratch);
			return -1;
		}

		if (fat32_fat_get(fs, cluster, &cluster) < 0) {
			slab_free(scratch);
			return -1;
		}
	}

	while (remaining > 0) {
		uint32_t sec;
		uint32_t soff;
		uint32_t n;

		if (!fat32_is_valid_cluster(fs, cluster))
			break;

		sec = fat32_cluster_to_sector(fs, cluster) +
		      coff / fs->bytes_per_sector;
		soff = coff % fs->bytes_per_sector;
		n = remaining < (uint64_t)(fs->bytes_per_sector - soff)
			? remaining
			: (fs->bytes_per_sector - soff);

		if (fat32_read_sector(fs, sec, scratch) < 0) {
			slab_free(scratch);
			return -1;
		}

		memcpy(out, scratch + soff, n);

		out += n;
		remaining -= n;
		coff += n;

		if (coff >= cluster_bytes) {
			coff = 0;

			if (fat32_fat_get(fs, cluster, &cluster) < 0) {
				slab_free(scratch);
				return -1;
			}
		}
	}

	slab_free(scratch);

	*out_len = count - remaining;

	return 0;
}

/*
 * 把数据写到文件偏移 pos 处，必要时扩展簇链。
 * 若 pos 超过原文件大小，缺口用 0 填充。
 * 成功后更新 inode->size 并回写目录项。
 */
static int fat32_write_data(struct inode* inode,
			    uint64_t pos,
			    const void* buf,
			    uint32_t count,
			    uint32_t* out_len)
{
	struct fat32_fs_priv* fs = inode->sb->private;
	struct fat32_inode_priv* priv = inode->private;
	uint32_t cluster_bytes = fat32_cluster_bytes(fs);
	uint8_t* scratch;
	const uint8_t* ub = buf;
	uint64_t old_size = inode->size;
	uint64_t end = pos + count;
	uint64_t start;
	uint64_t f;
	uint32_t cur;
	uint32_t prev;
	uint32_t coff;

	/*
	 * scratch 必须 slab 分配：块设备 DMA 要求低物理地址，
	 * 系统调用上下文的内核栈在高虚拟地址，不能直接给驱动。
	 */
	scratch = slab_alloc(fs->bytes_per_sector);

	if (scratch == NULL)
		return -1;

	if (fs->bytes_per_sector > FAT32_SECTOR_SIZE) {
		slab_free(scratch);
		return -1;
	}

	/*
	 * pos > old_size 时需要先把 [old_size, pos) 填 0，
	 * 因此实际写入范围从 min(pos, old_size) 开始。
	 */
	start = pos < old_size ? pos : old_size;

	cur = priv->first_cluster;
	prev = 0;
	coff = start % cluster_bytes;
	f = start;

	/* 先推进到 start 所在簇 */
	for (uint64_t i = 0; i < start / cluster_bytes; i++) {
		if (cur == 0 || fat32_is_chain_end(cur)) {
			/* 不应发生：范围起点在旧数据内 */
			slab_free(scratch);
			return -1;
		}

		if (fat32_fat_get(fs, cur, &cur) < 0) {
			slab_free(scratch);
			return -1;
		}
	}

	while (f < end) {
		uint32_t sec;
		uint32_t soff;
		uint64_t n;

		if (cur == 0 || fat32_is_chain_end(cur) ||
		    !fat32_is_valid_cluster(fs, cur)) {
			uint32_t nc;

			if (fat32_alloc_cluster(fs, &nc) < 0) {
				slab_free(scratch);
				return -1;
			}

			if (prev == 0)
				priv->first_cluster = nc;
			else
				fat32_fat_set(fs, prev, nc);

			cur = nc;
		}

		sec = fat32_cluster_to_sector(fs, cur) +
		      coff / fs->bytes_per_sector;
		soff = coff % fs->bytes_per_sector;
		n = (end - f) < (uint64_t)(fs->bytes_per_sector - soff)
			? (end - f)
			: (fs->bytes_per_sector - soff);

		if (soff == 0 && n == fs->bytes_per_sector && f >= pos) {
			/* 整扇区用户数据，直写 */
			if (fat32_write_sector(fs, sec, ub + (f - pos)) < 0) {
				slab_free(scratch);
				return -1;
			}
		} else {
			/*
			 * 非对齐或含 0 填充缺口：读-改-写。
			 * 虚拟数据源：f < pos 且 f >= old_size 的部分填 0，
			 * 其余取用户缓冲。
			 */
			if (fat32_read_sector(fs, sec, scratch) < 0) {
				slab_free(scratch);
				return -1;
			}

			for (uint64_t i = 0; i < n; i++) {
				uint64_t fo = f + i;

				if (fo < pos)
					scratch[soff + i] = 0;
				else
					scratch[soff + i] = ub[fo - pos];
			}

			if (fat32_write_sector(fs, sec, scratch) < 0) {
				slab_free(scratch);
				return -1;
			}
		}

		f += n;
		coff += n;

		if (coff >= cluster_bytes) {
			coff = 0;
			prev = cur;

			if (fat32_fat_get(fs, cur, &cur) < 0) {
				slab_free(scratch);
				return -1;
			}

			if (fat32_is_chain_end(cur) && f < end)
				cur = 0; /* 下轮分配新簇 */
		} else {
			prev = cur;
		}
	}

	if (end > inode->size) {
		inode->size = end;

		if (fat32_dirent_update_size(fs, priv, inode->size) < 0) {
			slab_free(scratch);
			return -1;
		}
	}

	*out_len = count;

	return 0;
}

/*
 * 截断/扩展文件到 size。
 * 截断释放多余簇；扩展追加零填充簇。
 * 成功后更新 inode->size 并回写目录项。
 */
static int fat32_truncate(struct inode* inode, uint64_t size)
{
	struct fat32_fs_priv* fs;
	struct fat32_inode_priv* priv;
	uint32_t cluster_bytes;
	uint32_t old_first;
	uint32_t old_n;
	uint32_t new_n;

	if (inode == NULL || inode->sb == NULL || inode->sb->private == NULL ||
	    inode->private == NULL)
		return -1;

	if ((inode->mode & S_IFMT) != S_IFREG)
		return -1;

	if (size > 0xFFFFFFFFULL)
		return -1;

	fs = inode->sb->private;
	priv = inode->private;
	cluster_bytes = fat32_cluster_bytes(fs);

	old_first = priv->first_cluster;
	old_n = old_first != 0 ? fat32_chain_len(fs, old_first) : 0;
	new_n = (uint32_t)((size + cluster_bytes - 1) / cluster_bytes);

	if (new_n == 0) {
		if (old_first != 0 && fat32_free_chain(fs, old_first) < 0)
			return -1;

		priv->first_cluster = 0;
	} else if (new_n > old_n) {
		/* 追加簇 */
		uint32_t first = old_first;
		uint32_t tail = old_first;

		while (fat32_is_valid_cluster(fs, tail)) {
			uint32_t next;

			if (fat32_fat_get(fs, tail, &next) < 0)
				return -1;

			if (fat32_is_chain_end(next))
				break;

			tail = next;
		}

		if (!fat32_is_valid_cluster(fs, tail) && old_first != 0)
			return -1;

		if (fat32_append_clusters(fs, &first, &tail, new_n - old_n) < 0)
			return -1;

		priv->first_cluster = first;
	} else if (new_n < old_n) {
		/* 截断：保留 new_n 个簇，其余释放 */
		uint32_t c = old_first;
		uint32_t next;

		for (uint32_t i = 0; i < new_n - 1; i++) {
			if (fat32_fat_get(fs, c, &c) < 0)
				return -1;
		}

		if (fat32_fat_get(fs, c, &next) < 0)
			return -1;

		if (fat32_fat_set(fs, c, FAT32_EOF_MARK) < 0)
			return -1;

		if (fat32_free_chain(fs, next) < 0)
			return -1;
	}

	inode->size = size;

	return fat32_dirent_update_size(fs, priv, (uint32_t)size);
}

/* ============================================================
 * file_operations
 * ============================================================ */

static int64_t fat32_read(struct file* file, void* buf, uint64_t count)
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

	if (fat32_read_data(inode, file->pos, buf, count, &out_len) < 0)
		return -1;

	/* file->pos 由 VFS 层推进 */
	return out_len;
}

static int64_t fat32_write(struct file* file, const void* buf, uint64_t count)
{
	struct inode* inode;
	uint32_t out_len = 0;

	if (file == NULL || file->inode == NULL || buf == NULL)
		return -1;

	inode = file->inode;

	if ((inode->mode & S_IFMT) != S_IFREG)
		return -1;

	if (inode->mode & 0444 && !(inode->mode & 0222))
		return -1; /* 只读属性 */

	if (count == 0)
		return 0;

	if (count > 0xFFFFFFFFULL)
		count = 0xFFFFFFFFULL;

	if (fat32_write_data(inode, file->pos, buf, count, &out_len) < 0)
		return -1;

	/* file->pos 由 VFS 层推进 */
	return out_len;
}

static int64_t fat32_seek(struct file* file, int64_t offset, int whence)
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

	if ((uint64_t)npos > inode->size)
		npos = inode->size;

	/* 新位置由 VFS 写回 file->pos */
	return npos;
}

static int fat32_close(struct file* file)
{
	(void)file;
	return 0;
}

/* ============================================================
 * super_block
 * ============================================================ */

static int fat32_get_super(struct filesystem* fs,
			   struct block_device* dev,
			   struct super_block** out)
{
	struct fat32_bpb* bpb;
	struct fat32_fs_priv* priv;
	struct super_block* sb;
	struct inode* root;
	uint8_t* sec;
	uint32_t total_sectors;
	uint32_t cluster_count;
	int ret = -1;

	if (fs == NULL || dev == NULL || out == NULL)
		return -1;

	sec = slab_alloc(FAT32_SECTOR_SIZE);

	if (sec == NULL)
		return -1;

	if (dev->driver.read(dev->private_data, 0, sec) < 0)
		goto free_sec;

	bpb = (struct fat32_bpb*)sec;

	/*
	 * FAT32 判别：
	 *   - root_entries == 0 且 sectors_per_fat16 == 0
	 *   - 合理的扇区/簇参数
	 */
	if (bpb->bpb_root_entries != 0 || bpb->bpb_sectors_per_fat != 0)
		goto free_sec;

	if (bpb->bpb_bytes_per_sector < 512 ||
	    bpb->bpb_bytes_per_sector > 4096 ||
	    (bpb->bpb_bytes_per_sector & (bpb->bpb_bytes_per_sector - 1)) != 0)
		goto free_sec;

	if (bpb->bpb_sectors_per_cluster == 0 ||
	    (bpb->bpb_sectors_per_cluster &
	     (bpb->bpb_sectors_per_cluster - 1)) != 0)
		goto free_sec;

	if (bpb->bpb_reserved_sectors == 0 || bpb->bpb_num_fats == 0)
		goto free_sec;

	total_sectors = bpb->bpb_total_sectors != 0
			    ? bpb->bpb_total_sectors
			    : bpb->bpb_total_sectors_large;

	if (total_sectors == 0 || bpb->bpb_sectors_per_fat32 == 0)
		goto free_sec;

	cluster_count = (total_sectors - bpb->bpb_reserved_sectors -
			 bpb->bpb_num_fats * bpb->bpb_sectors_per_fat32) /
			bpb->bpb_sectors_per_cluster;

	/* FAT32 规范要求最少 65525 簇 */
	if (cluster_count < 65525 || bpb->bpb_root_cluster < 2)
		goto free_sec;

	priv = slab_alloc(sizeof(*priv));

	if (priv == NULL)
		goto free_sec;

	memset(priv, 0, sizeof(*priv));

	priv->bdev = dev;
	priv->bytes_per_sector = bpb->bpb_bytes_per_sector;
	priv->sectors_per_cluster = bpb->bpb_sectors_per_cluster;
	priv->reserved_sectors = bpb->bpb_reserved_sectors;
	priv->num_fats = bpb->bpb_num_fats;
	priv->fat_sectors = bpb->bpb_sectors_per_fat32;
	priv->total_sectors = total_sectors;
	priv->fat_start = bpb->bpb_reserved_sectors;
	priv->data_start = bpb->bpb_reserved_sectors +
			   bpb->bpb_num_fats * bpb->bpb_sectors_per_fat32;
	priv->root_cluster = bpb->bpb_root_cluster;
	priv->cluster_count = cluster_count;

	sb = slab_alloc(sizeof(*sb));

	if (sb == NULL) {
		slab_free(priv);
		goto free_sec;
	}

	memset(sb, 0, sizeof(*sb));

	sb->fs = fs;
	sb->dev = dev;
	sb->private = priv;

	root = fat32_root_inode(sb);

	if (root == NULL) {
		slab_free(sb);
		slab_free(priv);
		goto free_sec;
	}

	sb->root = root;

	*out = sb;
	ret = 0;

free_sec:
	slab_free(sec);
	return ret;
}

static void fat32_kill_sb(struct super_block* sb)
{
	if (sb == NULL)
		return;

	/*
	 * 释放 super_block 持有的 root 引用。
	 * 若 cache 仍持有 root（umount 已先移除 cache，正常不会），
	 * inode_put 只会把引用减到 cache 的份额，不会误删。
	 */
	inode_put(sb->root);

	if (sb->private != NULL)
		slab_free(sb->private);

	slab_free(sb);
}

/*
 * 注册
 *  */

static struct filesystem fat32_fs = {
    .name = "fat32",
    .get_super = fat32_get_super,
    .kill_sb = fat32_kill_sb,
    .fops = &fat32_file_ops,
    .iops = &fat32_inode_ops,
};

static struct inode_operations fat32_inode_ops = {
    .lookup = fat32_lookup,
    .create = fat32_create,
    .mkdir = fat32_mkdir,
    .rmdir = fat32_rmdir,
    .unlink = fat32_unlink,
    .readdir = fat32_readdir,
    .getattr = fat32_getattr,
    .truncate = fat32_truncate,
    .destroy = fat32_inode_destroy,
};

static struct file_operations fat32_file_ops = {
    .read = fat32_read,
    .write = fat32_write,
    .seek = fat32_seek,
    .close = fat32_close,
};

void init_fat32(void)
{
	if (vfs_register_filesystem(&fat32_fs) < 0)
		printk("fat32: register filesystem failed\n");
	else
		printk("fat32: filesystem registered\n");
}
