# ext2 文件系统实现

## 一、概述

loeux-riscv 的 ext2 是一个最精简的 ext2 rev 0/1 实现，对接 [VFS 开发范式](../paradigm/vfs.md)。支持：

- 块大小 1024 / 2048 / 4096
- 12 direct + 1 single indirect + 1 double indirect（不支持 triple indirect）
- 变长目录项（linear linked-list）
- 读写、create / mkdir / unlink / rmdir / truncate / readdir
- 简单 bitmap 分配（first-fit，从 group 0 起线性扫描）
- inode 缓存：priv 中保存完整磁盘 inode 副本，写穿（write-through）

尚未实现：

- symlink、字符/块设备文件、socket、FIFO
- extended attributes、ACL
- journaling
- triple indirect block（`i_block[14]`）
- 目录项合并的"前驱合并"在所有路径上不一定完整（删除时仅与同块前一项合并）
- 严格按照 `i_links_count` 进行 inode 释放决策（仍做防御性 empty 扫描）

## 二、源码结构

| 文件 | 角色 |
|------|------|
| [include/fs/ext2.h](file:///home/loe/work/os/loeux-riscv/include/fs/ext2.h) | 公共头：磁盘常量、`ext2_fs_priv`、`ext2_inode_priv`、`init_ext2` 原型 |
| [fs/ext2.c](file:///home/loe/work/os/loeux-riscv/fs/ext2.c) | 实现：磁盘结构、block I/O、bitmap、inode 读写、目录遍历、VFS ops |
| [kernel/start.c](file:///home/loe/work/os/loeux-riscv/kernel/start.c#L58-L61) | 注册入口：`init_ext2()` 在 `init_fat32()` 之后调用 |
| [Makefile](file:///home/loe/work/os/loeux-riscv/Makefile#L50) | `fs/ext2.o` 已加入 OBJS 列表 |

`fs/Makefile` 使用 `wildcard *.c`，新增 `ext2.c` 自动纳入构建，无需修改子 Makefile。

## 三、磁盘结构（最小子集）

### 1. Superblock

固定在字节偏移 1024 处，跨 2 个 512 字节扇区。实现只解析以下字段：

```
s_inodes_count, s_blocks_count, s_free_inodes_count, s_free_blocks_count
s_first_data_block, s_log_block_size
s_blocks_per_group, s_inodes_per_group
s_magic (0xEF53), s_state, s_rev_level
s_first_ino, s_inode_size (rev 1，偏移 88；决定 inode 表定位)
```

`s_log_block_size` 解读方式：`block_size = 1024 << s_log_block_size`。

`first_data_block`：当 `block_size == 1024` 时为 1，否则为 0。

### 2. Block Group Descriptor

每组描述符 32 字节。实现使用以下字段：

```
bg_block_bitmap   块位图所在块号
bg_inode_bitmap   inode 位图所在块号
bg_inode_table    inode 表起始块号
bg_free_blocks_count
bg_free_inodes_count
bg_used_dirs_count  (未使用)
```

BGDT 起始块 = `ext2_sb_block + 1`。`ext2_sb_block` 在 1K 块时为 1，否则为 0。

### 3. Inode

磁盘 inode 大小：rev 0 固定 128 字节；rev 1 由超级块偏移 88 的 `s_inode_size` 指定（现代 mkfs.ext2 即使 1K 块也默认写 256）。实现只解析前 **128** 字节基础部分（`_Static_assert(sizeof==128)` 编译期自检），扩展尾在写回时保留不动。使用字段：

```
偏移 0   i_mode, i_uid, i_size
偏移 8   i_atime, i_ctime, i_mtime, i_dtime
偏移 24  i_gid, i_links_count
偏移 28  i_blocks       // 占用的 512 字节扇区数（不是逻辑块数！），不能漏，漏了后续字段全错位
偏移 32  i_flags        // 解析但不使用
偏移 36  i_osd1         // 保留，必须占位
偏移 40  i_block[15]
```

`i_block` 解读：
- `[0..11]` direct blocks
- `[12]` single indirect
- `[13]` double indirect
- `[14]` triple indirect（**不支持**，超过 double indirect 容量的文件读写会失败）

`i_blocks` 在 write / truncate / mkdir 等改变数据块集合的路径由 `ext2_refresh_i_blocks()`
按实际已分配块重新统计（非 hole 计数 × `block_size/512`），保证与 mke2fs/e2fsck 的口径一致。

容量上限（block_size = 1024）：约 64 MB
容量上限（block_size = 4096）：约 4 GB

### 4. Directory Entry

变长结构，按 4 字节对齐：

```c
struct ext2_dirent {
    uint32_t inode;       // 0 = 已删除/空闲槽位
    uint16_t rec_len;    // 本条目总长度
    uint8_t  name_len;
    uint8_t  file_type;  // 1=REG, 2=DIR, 7=SYMLINK
    char     name[];     // 不以 NUL 结尾
};
```

## 四、VFS 接入

遵循 [VFS 开发范式](../paradigm/vfs.md) 的"三不"原则：

1. **不做权限校验** — `inode->mode` 中只填 `S_IFREG` / `S_IFDIR`，rwx 位由 VFS 层覆盖
2. **不做并发互斥** — 文件系统层认为自身并发安全，所有元数据互斥由 `vfs_meta_lock` 序列化
3. **不管理 inode 生命周期** — `lookup` / `create` / `mkdir` 返回的 inode `refcount = 1`（归 caller），`destroy` 在 `refcount = 0` 时由 VFS 调用

### 注册

```c
static struct filesystem ext2_fs = {
    .name      = "ext2",
    .get_super = ext2_get_super,
    .kill_sb   = ext2_kill_sb,
    .fops      = &ext2_file_ops,
    .iops      = &ext2_inode_ops,
};

void init_ext2(void) {
    vfs_register_filesystem(&ext2_fs);
}
```

### 挂载

[kernel/start.c](file:///home/loe/work/os/loeux-riscv/kernel/start.c#L84) 当前把根挂载为 ext2：

```c
if (vfs_mount(&virtio_block_device, "/", "ext2") == -1)
        panic(...);
```

`ext2` 文件系统在 `init_vfs()` 之后通过 `init_ext2()` 注册；挂载失败（magic 不符、root 不是目录等）直接 panic。

## 五、关键实现细节

### 1. inode 私有数据

```c
struct ext2_inode_priv {
    uint32_t ino;
    uint32_t i_uid, i_gid, i_size, i_mode;
    uint32_t i_links_count;
    uint32_t i_blocks;   /* 512 字节扇区计数，写回时与磁盘同步 */
    uint32_t i_atime, i_ctime, i_mtime;
    uint32_t i_block[EXT2_N_BLOCKS];
};
```

priv 持有完整磁盘 inode 副本，修改时直接改 priv，并通过 `ext2_priv_save()` 写回磁盘。

### 2. 块映射

`ext2_get_block(priv, fs, lblk, allocate, &phys)`：

- `allocate == 0`：仅查询。hole（块指针为 0）返回 `phys = 0`
- `allocate == 1`：按需分配新块并清零，更新 priv 中的指针

间接块的读写通过 `ext2_ind_get` / `ext2_ind_set` 完成。double indirect 嵌套一层间接查询。

### 3. 目录项追加

`ext2_dir_add` 优先级：

1. 扫描现有块，找空闲槽位（`inode == 0` 且 `rec_len >= need`）：分割该槽位，头部写新项，尾部仍作为空闲项
2. 在块内最后一个 entry 之后追加：若其尾部剩余空间 ≥ need，则把尾部拆给新项
3. 都不行则扩展一个新数据块，新项占据整块（`rec_len = block_size`），同时更新 `dir->i_size`

### 4. 目录项删除

`ext2_dir_remove`：找到匹配 entry 后，将 `inode` 置 0。若 entry 不是块内第一个，则把其 `rec_len` 合并到同块前一项（按 `rec_len` 链找到上一个）。这样空间可在下次 `dir_add` 中复用。

### 5. 文件截断

- 截断到更小：从 `new_blocks` 到 `old_blocks` 释放 direct 区段的物理块；若截到 0，调用 `ext2_free_all_blocks` 释放 direct + indirect + double indirect 全链
- 截断到更大：不预分配，write 时按需分配（产生 sparse hole，read 返回 0）

### 6. 目录初始化

`ext2_mkdir` 后调用 `ext2_dir_init_dot`：

- 在新目录的 block 0 写入 `.`（指向自己）和 `..`（指向父目录）
- `..` 的 `rec_len` 延伸到块尾，符合 ext2 规范
- 父目录 `i_links_count++`（因新目录通过 `..` 引用父目录）
- 新目录 `i_links_count = 2`（自身 + `.`）

### 7. 时间戳

`ext2_now()` 复用 FAT32 的近似 unix 秒：

```c
return (uint32_t)(get_sys_timer_tick() / 100) + 1700000000U;
```

ext2 的 `i_atime` / `i_ctime` / `i_mtime` 都是 32-bit unix 秒，与 VFS `vfs_kstat.atime` 等直接对应。

### 8. 块缓存（write-back buffer cache）

**问题**：无缓存时，`ext2_read_block` / `ext2_write_block` 每次都同步走 virtio；一个 4K 块要 8 次 512B 扇区往返。一次 `mkdir` 会反复读写同一批元数据块（inode bitmap、block bitmap、inode 表块、父目录块）——经实测会卡顿很久。

**方案**：在 `struct ext2_fs_priv` 上挂一张 hashmap（`utils/hashmap`，键 = 块号 `uint32_t`，值 = `ext2_cache_entry{block, data, dirty}`）：

- `ext2_read_block`：命中则 `memcpy` 返回；未命中走 `ext2_disk_read_raw` 读入缓存再 `memcpy`。
- `ext2_write_block`：只写缓存并标记 `dirty`，不立即落盘。
- `ext2_cache_flush`：在 `mkdir` / `create` / `unlink` / `rmdir` / `truncate` / `write` 等顶层修改操作**成功返回前**一次性把所有 dirty 块写回磁盘。
- 只读路径（`lookup` / `readdir`）不写盘，纯读缓存，命中即返回。

**死锁 / 调度陷阱（实际踩过：`mkdir` 触发 `sched: noff != 1` panic）**：
`hash_table_foreach` 回调全程持有 `ht->lock` 自旋锁（持锁 = `push_off`，中断关闭）。
而 virtio 同步 IO（`virtio_disk_rw_sync`）在轮询完成前会**主动 `intr_on()` 重新开中断**，
轮询期间可能被时钟中断抢占并进入 `sched()`。若回调内做磁盘写，进程进入调度器时
`noff == 2`（ht->lock 的一次 + 进程锁的一次），触发 [kernel/proc.c](file:///home/loe/work/os/loeux-riscv/kernel/proc.c)
的 `sched: noff != 1` panic。

因此 flush 采用 **gather-then-write** 两段式：

1. `hash_table_foreach` 回调内**只收集** dirty 条目指针到数组并清 `dirty`，绝不做 IO；
2. foreach 返回（`ht->lock` 已释放）后，再调 `ext2_disk_write_raw` 逐块写盘；写失败重新置 `dirty` 留待下次。

条目较多（数组超过 slab 单次 4096B 上限）时分批循环；一批全部写失败则终止避免设备故障时死循环。
另外缓存"未命中读盘"路径也必须在 `hash_table_lookup` **返回之后**（锁已释放）才调
`ext2_disk_read_raw`，不能持锁读盘。

**退化**：缓存初始化失败或 slab 分配失败时，`ext2_read_block` / `ext2_write_block` 自动退化到 raw 直读直写，保证功能正确（只是慢）。

**缓存生命周期**：`get_super` 中 `priv` 字段设好后、第一次 `ext2_inode_read` 前调 `ext2_cache_init`；`kill_sb` 中先 `ext2_cache_flush`（落盘 dirty）再 `ext2_cache_destroy`（释放所有条目与 hashmap）。各错误回滚路径也调 `ext2_cache_destroy` 避免泄漏。

## 六、限制与已知缺陷

| 项 | 说明 |
|----|------|
| triple indirect | 不支持，超大文件读写失败 |
| inode size | rev 0 固定 128；rev 1 解析超级块 `s_inode_size`（通常 256），仅读写前 124 字节基础 inode，扩展尾保留不动 |
| symlink | 不支持；lookup 不会跟随符号链接（VFS 层目前也未实现） |
| 目录项合并 | 删除时仅与同块前一项合并；跨块的"洞"不收缩 |
| 写一致性 | 不维护 `s_state`（clean / dirty）；崩溃后 fsck 必须从位图重建 |
| 权限位 | 文件系统层只填 `S_IFREG` / `S_IFDIR`，依赖 VFS 层覆盖 rwx |
| 并发 | 文件系统层不做互斥，依赖 VFS `vfs_meta_lock` 与 `file->slk` |

## 七、构建与测试

构建验证：

```
make kernel.bin
```

输出 `fs/ext2.o` 与 `kernel.bin`，编译过程无警告（`-Wall -Werror`）。

由于当前 `loeux.img` 仍是 FAT32 镜像，ext2 目前只完成注册阶段，未在运行时挂载。切换镜像为 ext2 后，修改 [kernel/start.c:82](file:///home/loe/work/os/loeux-riscv/kernel/start.c#L82) 把 `"fat32"` 改为 `"ext2"` 即可启用。

## 八、参考

- [doc/paradigm/vfs.md](../paradigm/vfs.md) — VFS 接入新文件系统的开发范式
- [fs/fat32.c](file:///home/loe/work/os/loeux-riscv/fs/fat32.c) — FAT32 实现，作为 ext2 的结构参考
- [fs/vfs.c](file:///home/loe/work/os/loeux-riscv/fs/vfs.c) — VFS 层，负责路径规范化、权限校验、并发保护
- ext2 disk structure: `e2fsprogs` 源码 `libext2fs/ext2_fs.h`
