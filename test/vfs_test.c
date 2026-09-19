#include <vfs.h>
#include <virtio_mmio.h>
#include <test.h>
#include <fat32.h>
#include <printk.h>
#include <panic.h>
#include <proc.h>
#include <memory.h>
#include <lib.h>

/*
 * 新版 VFS 接口测试
 *
 * 打开文件的标准姿势：
 *   fd = vfs_open(path, flags);
 *   f  = fd_get(fd);        // 临时引用
 *   vfs_read / vfs_write / vfs_seek(f, ...)
 *   file_put(f);            // 释放临时引用
 *   fd_close(fd);           // 释放 fd 引用
 */

static void print_mount_tree(struct mount *mnt, int depth)
{
        struct mount *c;

        if (mnt == NULL)
                return;

        for (int i = 0; i < depth; i++)
                printk("    ");

        printk("%-20s fs=%-8s sb=%0#lx\n",
               mnt->path,
               (mnt->sb && mnt->sb->fs) ? mnt->sb->fs->name : "?",
               (unsigned long)(mnt->sb ? mnt->sb : 0));

        for (c = mnt->child; c != NULL; c = c->next)
                print_mount_tree(c, depth + 1);
}

void print_mount_table(void)
{
        printk("\n==================== Mount Table ====================\n");
        print_mount_tree(vfs_get_root_mount(), 0);
        printk("=====================================================\n");
}

// offset处读取文件n个字节
void vfs_test_seek_file(const char *path, int offset, uint64_t n)
{
        int fd;
        struct file *f;
        char *buf;
        int64_t rd;

        fd = vfs_open(path, O_RDONLY);

        if (fd < 0)
        {
                printk("vfs_test_seek_file: open %s failed\n", path);
                return;
        }

        f = fd_get(fd);
        buf = alloc_page();

        if (f != NULL && buf != NULL &&
            vfs_seek(f, offset, SEEK_SET) >= 0)
        {
                rd = vfs_read(f, buf, n);

                printk("从 %d 开始, 读取文件 %s 的 %lu 个字节, 实际读取 %ld\n",
                       offset, path, n, rd);

                for (int64_t i = 0; i < rd && i < 64; i++)
                        printk("%0#lx ", (unsigned char)buf[i]);

                printk("\n");
        }

        if (buf)
                free_page(buf);
        if (f)
                file_put(f);

        fd_close(fd);
}

void vfs_test_read_file(const char *path)
{
        int fd;
        struct file *f;
        char *buf;
        int64_t n;
        uint64_t total = 0;
        uint64_t times = 0;

        fd = vfs_open(path, O_RDONLY);

        if (fd < 0)
        {
                printk("vfs_test_read_file: open %s failed\n", path);
                return;
        }

        f = fd_get(fd);
        buf = alloc_page();

        if (f != NULL && buf != NULL)
        {
                while ((n = vfs_read(f, buf, 4096)) > 0)
                {
                        total += n;
                        times++;
                }

                printk("读取 %s: 共 %lu 次, 共计 %lu 字节\n",
                       path, times, total);
        }

        if (buf)
                free_page(buf);
        if (f)
                file_put(f);

        fd_close(fd);
}

/* 写入/读回校验一块数据 */
static int test_write_readback(const char *path,
                               const char *data,
                               uint32_t len)
{
        int fd;
        struct file *f;
        char *buf;
        int64_t n;
        int ok = 0;

        fd = vfs_open(path, O_RDONLY);

        if (fd < 0)
        {
                printk("  reopen %s failed\n", path);
                return 0;
        }

        f = fd_get(fd);
        buf = alloc_page();

        if (f != NULL && buf != NULL)
        {
                n = vfs_read(f, buf, len + 16);

                if (n == (int64_t)len &&
                    memcmp(buf, data, len) == 0)
                {
                        ok = 1;
                }
                else
                {
                        printk("  readback len=%ld (want %u)\n", n, len);
                }
        }

        if (buf)
                free_page(buf);
        if (f)
                file_put(f);

        fd_close(fd);

        return ok;
}

void test_fat32_operations(void)
{
        const char *msg = "Hello FAT32! loeux riscv vfs test.";
        const uint32_t msg_len = sizeof("Hello FAT32! loeux riscv vfs test.") - 1;
        int fd;
        struct file *f;
        struct inode *ino = NULL;
        int64_t n;

        printk("\n========== FAT32 File Operation Test ==========\n");

        printk("[Test 1] create /T1.TXT and write\n");

        if (vfs_create("/T1.TXT", 0644, &ino) != 0)
        {
                printk("  vfs_create failed\n");
        }
        else
        {
                /* create 返回的引用归 caller，校验后立即释放 */
                printk("  create OK: size=%lu refcount=%u\n",
                       ino->size, ino->refcount);
                inode_put(ino);
        }

        fd = vfs_open("/T1.TXT", O_RDWR | O_TRUNC);

        if (fd < 0)
        {
                printk("  open /T1.TXT failed\n");
                return;
        }

        f = fd_get(fd);

        if (f == NULL)
        {
                fd_close(fd);
                return;
        }

        n = vfs_write(f, msg, msg_len);
        printk("  write: %ld bytes (want %u) %s\n",
               n, msg_len, n == (int64_t)msg_len ? "OK" : "FAIL");

        file_put(f);
        fd_close(fd);

        printk("[Test 2] read back /T1.TXT\n");

        if (test_write_readback("/T1.TXT", msg, msg_len))
                printk("  readback OK\n");
        else
                printk("  readback FAIL\n");

        printk("[Test 3] mkdir /TDIR and create child\n");

        if (vfs_mkdir("/TDIR", 0755) != 0)
                printk("  mkdir /TDIR failed\n");

        if (vfs_create("/TDIR/CHILD.TXT", 0644, &ino) != 0)
                printk("  create /TDIR/CHILD.TXT failed\n");
        else
                inode_put(ino);

        {
                struct inode *dir = NULL;

                if (vfs_lookup("/TDIR", &dir) == 0 && dir != NULL)
                {
                        printk("  lookup /TDIR OK: size=%lu refcount=%u\n",
                               dir->size, dir->refcount);
                        inode_put(dir);
                }
                else
                {
                        printk("  lookup /TDIR failed\n");
                }
        }

        printk("[Test 4] readdir /\n");

        fd = vfs_open("/", O_RDONLY | O_DIRECTORY);

        if (fd >= 0)
        {
                f = fd_get(fd);

                if (f != NULL)
                {
                        struct vfs_dirent de;

                        while (vfs_readdir(f, &de) == 0)
                        {
                                printk("  [%s] ino=%lu\n", de.name,
                                       (unsigned long)de.ino);
                        }

                        file_put(f);
                }

                fd_close(fd);
        }

        printk("[Test 5] unlink /T1.TXT\n");

        if (vfs_unlink("/T1.TXT") != 0)
                printk("  unlink failed\n");

        if (vfs_lookup("/T1.TXT", &ino) == 0)
        {
                printk("  FAIL: lookup after unlink succeeded\n");
                inode_put(ino);
        }
        else
                printk("  lookup after unlink fails as expected\n");

        printk("[Test 6] rmdir /TDIR\n");

        if (vfs_unlink("/TDIR/CHILD.TXT") != 0)
                printk("  unlink child failed\n");

        if (vfs_rmdir("/TDIR") != 0)
                printk("  rmdir failed (not empty?)\n");
        else
                printk("  rmdir OK\n");

        printk("========== FAT32 File Operation Test Complete ==========\n");
}

void test_umount(void)
{
        printk("\n========== Umount Test ==========\n");

        printk("[Test 1] umount / 应该失败\n");

        if (vfs_umount("/") == 0)
                printk("  FAIL: root umounted!\n");
        else
                printk("  OK: root umount rejected\n");

        printk("[Test 2] umount 不存在的挂载点应该失败\n");

        if (vfs_umount("/NO_SUCH_MOUNT") == 0)
                printk("  FAIL: unknown mount umounted!\n");
        else
                printk("  OK: unknown mount rejected\n");

        /*
         * 完整的 mount/umount 往返测试需要第二个块设备，
         * 单设备环境下通过 fail 路径验证接口行为。
         */

        printk("========== Umount Test Complete ==========\n");
}
