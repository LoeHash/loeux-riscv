#include <syscall.h>
#include <printk.h>
#include <vfs.h>
#include <lib.h>
#include <proc.h>

uint64_t sys_mkdir()
{
        struct task_struct *ts = get_task();
        char path[MAX_PATH_LEN];
        char u_path[MAX_PATH_LEN];
        uint64_t path_addr;

        get_arg_addr(0, &path_addr); // a0 = path

        if (copyinstr(ts->pg, u_path, path_addr, MAX_PATH_LEN) < 0)
        {
                return -1;
        }
        // 构建绝对路径
        do_build_user_path(path, u_path, ts->cwd);
        printk("mkdir: %s\n", path);

        file_attr_t attr = {.is_dir = 1, .readable = 1, .writable = 1};
        return vfs_create(path, attr);
}

uint64_t sys_read()
{
        int fd;
        uint64_t buf;
        uint64_t count;

        get_arg_int(0, &fd);
        get_arg_addr(1, &buf);
        get_arg_addr(2, &count);

        int max = PG_4K_SIZE > count ? count : PG_4K_SIZE;

        char *kbuf = (char *)kalloc();
        if (kbuf == NULL)
                return -1;

        int64_t ret = vfs_read(fd, kbuf, max);
        if (ret == -1)
        {
                kfree(kbuf);
                return -1;
        }

        copy_data_str_out(buf, kbuf, ret);
        kfree(kbuf);
        return ret;
}

uint64_t sys_close()
{
        int fd;

        get_arg_int(0, &fd);

        return vfs_close(fd);
}

uint64_t sys_open()
{
        struct task_struct *ts = get_task();
        char *path = kalloc();
        char *u_path = kalloc();
        uint64_t path_addr;
        uint32_t flags;
        int ret;

        get_arg_addr(0, &path_addr); // a0 = path
        get_arg_int(1, &flags);      // a1 = flags

        if (copyinstr(ts->pg, u_path, path_addr, MAX_PATH_LEN) < 0)
        {
                return -1;
        }
        // 构建绝对路径
        do_build_user_path(path, u_path, ts->cwd);

        // 现在我们有绝对路径了
        // 不存在
        if ((ret = vfs_open(path, flags)) < 0)
        {
                // 创建文件
                if (flags & FS_O_CREAT)
                {

                        file_attr_t attr = {.is_dir = 0, .readable = 0, .writable = 0};
                        if (flags & FS_O_READ)
                        {
                                attr.readable = 1;
                        }

                        if (flags & FS_O_WRITE)
                        {
                                attr.writable = 1;
                        }
                        ret = vfs_create(path, attr);
                        kfree(path);
                        kfree(u_path);
                        return ret;
                }
                kfree(path);
                kfree(u_path);
                return -1;
        }
        else
        {
                kfree(path);
                kfree(u_path);
                return ret;
        }
}

uint64_t sys_fstat()
{
        int fd;
        uint64_t ubuf;
        struct stat st;

        get_arg_int(0, &fd);
        get_arg_addr(1, &ubuf);

        if (ubuf == 0)
                return -1;

        if (vfs_fstat(fd, &st) < 0)
                return -1;

        if (copy_data_str_out(ubuf, (char *)&st, sizeof(st)) < 0)
                return -1;

        return 0;
}

/*
 * getdents(fd, dirp, count)：读目录项，返回填入的字节数，目录结束返回 0。
 * 与 sys_read 一样走单页内核 bounce buffer，再一次性 copyout：
 * 底层 FS 只操作内核地址，不直接碰用户页表。
 */
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

        char *kbuf = (char *)kalloc();
        if (kbuf == NULL)
                return -1;

        int64_t ret = vfs_getdents(fd, kbuf, max);
        if (ret < 0)
        {
                kfree(kbuf);
                return -1;
        }

        if (ret > 0 && copy_data_str_out(ubuf, kbuf, ret) < 0)
        {
                kfree(kbuf);
                return -1;
        }

        kfree(kbuf);
        return (uint64_t)ret;
}

uint64_t sys_write()
{
        int fd;
        uint64_t buf;
        uint64_t count;

        get_arg_int(0, &fd);
        get_arg_addr(1, &buf);
        get_arg_addr(2, &count);

        int max = PG_4K_SIZE > count ? count : PG_4K_SIZE;

        char *kbuf = (char *)kalloc();
        if (kbuf == NULL)
                return -1;

        copy_data_str(buf, kbuf, max);

        int ret = vfs_write(fd, kbuf, max);
        kfree(kbuf);
        return ret;
}