#include <syscall.h>
#include <printk.h>
#include <vfs.h>
#include <stat.h>
#include <dirent.h>
#include <lib.h>
#include <proc.h>
#include <slab.h>

uint64_t sys_read()
{
	int fd;
	uint64_t buf;
	uint64_t count;

	get_arg_int(0, &fd);
	get_arg_addr(1, &buf);
	get_arg_addr(2, &count);

	uint64_t max = count > PG_4K_SIZE ? PG_4K_SIZE : count;

	char* kbuf = (char*)kalloc();

	if (kbuf == NULL)
		return -1;

	struct file* file = fd_get(fd);

	if (file == NULL) {
		kfree(kbuf);
		return -1;
	}

	int64_t ret = vfs_read(file, kbuf, max);

	file_put(file);

	if (ret < 0) {
		kfree(kbuf);
		return -1;
	}

	if (copy_data_str_out(buf, kbuf, ret) < 0) {
		kfree(kbuf);
		return -1;
	}

	kfree(kbuf);

	return ret;
}

uint64_t sys_write()
{
	int fd;
	uint64_t buf;
	uint64_t count;

	get_arg_int(0, &fd);
	get_arg_addr(1, &buf);
	get_arg_addr(2, &count);

	uint64_t max = count > PG_4K_SIZE ? PG_4K_SIZE : count;

	char* kbuf = (char*)kalloc();

	if (kbuf == NULL)
		return -1;

	if (copy_data_str(buf, kbuf, max) < 0) {
		kfree(kbuf);
		return -1;
	}

	struct file* file = fd_get(fd);

	if (file == NULL) {
		kfree(kbuf);
		return -1;
	}

	int64_t ret = vfs_write(file, kbuf, max);

	file_put(file);

	kfree(kbuf);

	return ret;
}

uint64_t sys_close()
{
	int fd;

	get_arg_int(0, &fd);

	return fd_close(fd);
}

uint64_t sys_lseek()
{
	int fd;
	int offset;
	int whence;

	get_arg_int(0, &fd);

	get_arg_int(1, &offset);

	get_arg_int(2, &whence);

	struct file* file = fd_get(fd);

	if (file == NULL)
		return -1;

	int64_t ret = vfs_seek(file, offset, whence);

	file_put(file);

	return ret;
}

uint64_t sys_fstat()
{
	int fd;
	uint64_t stat_addr;

	get_arg_int(0, &fd);

	get_arg_addr(1, &stat_addr);

	struct file* file = fd_get(fd);

	if (file == NULL)
		return -1;

	struct vfs_kstat stat;

	int ret = vfs_fstat(file, &stat);

	file_put(file);

	if (ret < 0)
		return -1;

	/*
	 * 内核 struct vfs_kstat 与用户 struct stat 布局不同，
	 * 必须按字段转换后整体 copyout。
	 */
	struct stat ust;

	memset(&ust, 0, sizeof(ust));
	ust.st_ino = stat.ino;
	ust.st_mode = stat.mode;
	ust.st_nlink = stat.nlink;
	ust.st_uid = stat.uid;
	ust.st_gid = stat.gid;
	ust.st_size = (off_t)stat.size;
	ust.st_blksize = (blksize_t)stat.blksize;
	ust.st_blocks = (blkcnt_t)stat.blocks;
	ust.st_atime = (time_t)stat.atime;
	ust.st_mtime = (time_t)stat.mtime;
	ust.st_ctime = (time_t)stat.ctime;

	if (copy_data_str_out(stat_addr, (char*)&ust, sizeof(ust)) < 0)
		return -1;

	return 0;
}

uint64_t sys_getdents()
{
	int fd;
	uint64_t ubuf;
	uint64_t count;

	get_arg_int(0, &fd);

	get_arg_addr(1, &ubuf);

	get_arg_addr(2, &count);

	if (ubuf == 0)
		return -1;

	uint64_t max = count > PG_4K_SIZE ? PG_4K_SIZE : count;

	char* kbuf = (char*)kalloc();

	if (kbuf == NULL)
		return -1;

	struct file* file = fd_get(fd);

	if (file == NULL) {
		kfree(kbuf);
		return -1;
	}

	uint64_t offset = 0;

	/*
	 * 内核 struct vfs_dirent 与用户 struct dirent 布局不同
	 * （用户侧为 d_ino/d_off/d_reclen/d_type/d_name），
	 * 逐条转换成用户记录再 copyout，返回填入的字节数。
	 */
	while (offset + sizeof(struct dirent) <= max) {
		struct vfs_dirent vd;
		struct dirent* ud;

		int ret = vfs_readdir(file, &vd);

		/* vfs_readdir 成功返回 0，结束/失败返回 -1 */
		if (ret < 0)
			break;

		ud = (struct dirent*)(kbuf + offset);

		memset(ud, 0, sizeof(*ud));
		ud->d_ino = vd.ino;
		ud->d_off = (int64_t)vd.ino;
		ud->d_reclen = sizeof(struct dirent);
		/* vfs_dirent.type 为 S_IF*，用户 d_type 为 DT_* */
		ud->d_type = (uint8_t)IFTODT(vd.type);

		/* vd.name 无 NUL 保证，按定长拷贝后补终止符 */
		memcpy(ud->d_name, vd.name, VFS_NAME_MAX);
		ud->d_name[VFS_NAME_MAX] = '\0';

		offset += sizeof(struct dirent);
	}

	file_put(file);

	if (offset > 0) {
		if (copy_data_str_out(ubuf, kbuf, offset) < 0) {
			kfree(kbuf);
			return -1;
		}
	}

	kfree(kbuf);

	return offset;
}

uint64_t sys_open()
{
	struct task_struct* ts = get_task();

	uint64_t path_addr;
	uint32_t flags;

	get_arg_addr(0, &path_addr);

	get_arg_int(1, &flags);

	char* path = slab_alloc(MAX_PATH_LEN);

	if (path == NULL)
		return -1;

	if (copyinstr(ts->pg, path, path_addr, MAX_PATH_LEN) < 0) {
		slab_free(path);
		return -1;
	}

	/*
	 * vfs_open:
	 *
	 * 处理：
	 * 绝对路径
	 * 相对路径
	 * cwd inode
	 * inode cache
	 * file 创建
	 * fd 安装
	 */
	int ret = vfs_open(path, flags);

	slab_free(path);

	return ret;
}

uint64_t sys_mkdir()
{
	struct task_struct* ts = get_task();

	uint64_t path_addr;

	get_arg_addr(0, &path_addr);

	char* path = slab_alloc(MAX_PATH_LEN);

	if (path == NULL)
		return -1;

	if (copyinstr(ts->pg, path, path_addr, MAX_PATH_LEN) < 0) {
		slab_free(path);
		return -1;
	}

	int ret = vfs_mkdir(path, 0755);

	slab_free(path);

	return ret;
}