#include <syscall.h>
#include <printk.h>
#include <vfs.h>
#include <lib.h>
#include <proc.h>

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