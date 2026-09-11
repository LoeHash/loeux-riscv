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

        // 1. 从用户空间拷贝 path 字符串
        //    copyinstr 遍历用户页表翻译地址，遇到 \0 停止
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