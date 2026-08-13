#include <vfs.h>
#include <virtio.h>
#include <test.h>
#include <fat12.h>
#include <printk.h>
extern struct file *fd_table[MAX_FD_NUM];
extern struct mount_entry mount_points[MAX_MOUNT_NUM];

void print_mount_table(void)
{
        int i;
        int found = 0;

        printk("\n==================== Mount Table ====================\n");
        printk("%-4s %-30s %-12s %-12s\n", "Idx", "Mount Point", "Device", "FS Type");
        printk("---------------------------------------------------------\n");

        for (i = 0; i < MAX_MOUNT_NUM; i++)
        {
                struct mount_entry *entry = &mount_points[i];

                // 检查挂载点是否有效（假设空字符串表示未使用）
                if (entry->mount_point[0] == '\0')
                {
                        continue;
                }

                found = 1;

                // 打印索引和挂载点
                printk("%-4d %-30s ", i, entry->mount_point);

                // 打印设备地址
                if (entry->device != NULL)
                {
                        printk("%0#lx ", (unsigned long)entry->device);
                }
                else
                {
                        printk("%-12s ", "NULL");
                }

                // 打印文件系统类型（根据 fs_ops 地址判断，或者你可以添加 fs_type 字段）
                if (entry->fs_ops != NULL)
                {
                        printk("%0#lx", (unsigned long)entry->fs_ops);
                }
                else
                {
                        printk("%-12s", "NULL");
                }
                printk("\n");

                // 可选：打印更多详细信息
                printk("    fs_priv: ");
                if (entry->fs_priv != NULL)
                {
                        printk("%0#lx", (unsigned long)entry->fs_priv);
                }
                else
                {
                        printk("NULL");
                }
                printk("\n");

                // 如果有 block_device，打印其信息
                if (entry->device != NULL)
                {
                        printk("    sector_count: ");
                        uint64_t sectors = entry->device->driver.sector_count(entry->device->private_data);
                        printk("%ld\n", sectors);
                }
                printk("\n");
        }

        if (!found)
        {
                printk("No mount points found.\n");
        }

        printk("==================================\n");
}

void test_fat12_operations(void)
{
        char read_buf[512] = {0};
        const char *write_data = "Hello FAT12! This is a test write from kernel via VFS.";
        int fd, ret;
        int64_t bytes;
        struct file *file;

        printk("\n========== FAT12 File Operation Test ==========\n");

        printk("[Test 1] Listing root directory\n");
        struct fat12_priv *fs = (struct fat12_priv *)0x81ab2000;
        uint8_t *buf = alloc_page();
        if (buf)
        {
                for (uint32_t s = fs->root_dir_start; s < fs->root_dir_start + fs->root_dir_sectors; s++)
                {
                        fs->bdev->driver.read(fs->bdev->private_data, s, buf);
                        for (int i = 0; i < 16; i++)
                        {
                                struct fat12_dirent *e = (struct fat12_dirent *)(buf + i * 32);
                                if (e->dir_name[0] == 0x00)
                                {
                                        free_page(buf);
                                        goto test2;
                                }
                                if (e->dir_name[0] == 0xE5)
                                        continue;
                                if (e->dir_attr == 0x0F)
                                        continue;
                                if (e->dir_attr == 0x08)
                                        continue;
                                printk("  %.8s.%.3s size=%u cluster=%u\n",
                                       e->dir_name, e->dir_ext, e->dir_file_size, e->dir_first_cluster_low);
                        }
                }
                free_page(buf);
        }

test2:
        printk("\n[Test 2] Opening /hello.txt for read/write\n");
        fd = vfs_open("/hello.txt", FS_O_RW);
        if (fd < 0)
        {
                printk("  ✗ Failed to open /hello.txt (fd=%d)\n", fd);
                return;
        }
        printk("  ✓ File opened successfully, fd=%d\n", fd);

        printk("\n[Test 3] Reading from /hello.txt (first 16 bytes)\n");
        memset(read_buf, 0, sizeof(read_buf));
        bytes = vfs_read(fd, read_buf, 16);
        if (bytes < 0)
        {
                printk("  ✗ Read failed: %ld\n", bytes);
        }
        else
        {
                printk("  ✓ Read %ld bytes\n", bytes);
                printk("  Data: ");
                for (int i = 0; i < bytes && i < 16; i++)
                {
                        if (read_buf[i] >= 0x20 && read_buf[i] < 0x7F)
                                printk("%c", read_buf[i]);
                        else
                                printk(".");
                }
                printk("\n  Hex: ");
                for (int i = 0; i < bytes && i < 16; i++)
                {
                        printk("%0#lx ", (unsigned char)read_buf[i]);
                }
                printk("\n");
        }

        printk("\n[Test 4] Writing to /hello.txt (append)\n");
        file = fd_table[fd];
        if (file)
        {
                struct fat12_node *fnode = (struct fat12_node *)file->private;
                printk("  Current file size: %u bytes\n", fnode->file_size);
                file->pos = fnode->file_size;
        }
        bytes = vfs_write(fd, write_data, strlen(write_data));
        if (bytes < 0)
        {
                printk("  ✗ Write failed: %ld\n", bytes);
        }
        else
        {
                printk("  ✓ Write %ld bytes: \"%s\"\n", bytes, write_data);
        }

        printk("\n[Test 5] Reading back to verify write\n");
        if (file)
                file->pos = 0;
        memset(read_buf, 0, sizeof(read_buf));
        bytes = vfs_read(fd, read_buf, 512);
        if (bytes < 0)
        {
                printk("  ✗ Read back failed: %ld\n", bytes);
        }
        else
        {
                printk("  ✓ Read back %ld bytes\n", bytes);
                printk("  Full content:\n  ");
                for (int i = 0; i < bytes && i < 64; i++)
                {
                        if (read_buf[i] >= 0x20 && read_buf[i] < 0x7F)
                                printk("%c", read_buf[i]);
                        else
                                printk(".");
                }
                if (bytes > 64)
                        printk("...");
                printk("\n");
        }

        printk("\n[Test 6] Closing file\n");
        ret = vfs_close(fd);
        if (ret < 0)
        {
                printk("  ✗ Close failed: %d\n", ret);
        }
        else
        {
                printk("  ✓ File closed successfully\n");
        }

        printk("\n========== FAT12 File Operation Test Complete ==========\n");
}