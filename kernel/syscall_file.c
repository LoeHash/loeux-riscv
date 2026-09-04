#include <syscall.h>
#include <printk.h>
#include <vfs.h>
#include <lib.h>
#include <proc.h>

uint64_t sys_write()
{
        int fd;
        uint64_t buf;
        uint64_t count;
        char kbuf[128];
        int read = 0;
        get_arg_int(0, &fd);
        get_arg_addr(1, &buf);
        get_arg_addr(2, &count);
        int max = 128 > count ? count : 128;
        copy_data_str(buf, kbuf, max);

        // printk("the fd: %d\n", fd);

        return vfs_write(fd, kbuf, max);
}