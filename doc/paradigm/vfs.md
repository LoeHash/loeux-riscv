# VFS 层开发范式

## 一、VFS 层概览

| 功能 | 状态 | 说明 |
|------|------|------|
| mount/umount | **Active** | 挂载树管理、busy 检查、inode cache 清理 |
| lookup/lookup_parent | **Active** | 路径规范化（`.`/`..`/相对路径）、inode cache |
| open/close/dup | **Active** | O_CREAT/O_EXCL/O_TRUNC/O_DIRECTORY/O_APPEND、字符设备 |
| read/write/seek/readdir | **Active** | file->slk 串行化、VFS 管理偏移 |
| create/mkdir/unlink/rmdir | **Active** | 权限校验、并发保护、权限位设置 |
| truncate/fstat | **Active** | 权限校验、并发保护 |
| inode 生命周期 | **Active** | refcount + inode cache + destroy |
| 权限校验 | **Active** | VFS 层统一拦截（uid/gid/rwx） |

## 二、设计思想

```
┌──────────────────────────────────────────────────────┐
│                    VFS Layer                          │
│  ┌─────────┐  ┌──────────┐  ┌──────────┐             │
│  │路径规范化│  │权限校验   │  │并发保护    │            │
│  │normalize │  │permission│  │meta_lock  │            │
│  └─────────┘  └──────────┘  └──────────┘             │
│  ┌─────────────────────────────────────┐             │
│  │ inode cache (hashmap)                │             │
│  │ file refcount / fd table             │             │
│  └─────────────────────────────────────┘             │
├──────────────────────────────────────────────────────┤
│              Filesystem Layer (如 fat32)               │
│  ┌─────────────────────────────────────────────┐      │
│  │ 只负责磁盘读写，认为自身并发安全               │      │
│  │ 只填写文件类型 (S_IFREG / S_IFDIR)           │      │
│  │ 不处理权限校验、不做并发互斥                   │      │
│  └─────────────────────────────────────────────┘      │
├──────────────────────────────────────────────────────┤
│              Block Device Layer                        │
│  virtio-blk driver                                     │
└──────────────────────────────────────────────────────┘
```

## 三、接入新文件系统的开发范式

要接入一个新文件系统，只需实现以下接口并注册：

### 1. 文件系统描述符

```c
struct filesystem my_fs = {
    .name    = "myfs",
    .get_super = my_get_super,   // 解析超级块，创建 root inode
    .kill_sb  = my_kill_sb,     // 释放 root inode 引用与 sb
    .fops    = &my_file_ops,     // file_operations
    .iops    = &my_inode_ops,    // inode_operations
};
```

通过 `vfs_register_filesystem(&my_fs)` 注册，`vfs_mount(dev, "/", "myfs")` 挂载。

### 2. inode_operations（文件系统层必须实现）

| 接口 | 职责 | 权限/并发 |
|------|------|-----------|
| `lookup(dir, name, &inode)` | 在目录中查找名字，返回新 inode（refcount=1） | VFS 已校验 MAY_EXEC |
| `create(dir, name, mode, &inode)` | 创建普通文件，设置 S_IFREG | VFS 已校验权限 + 加 meta_lock |
| `mkdir(dir, name, mode, &inode)` | 创建目录，设置 S_IFDIR | 同上 |
| `unlink(dir, name)` | 删除目录项 + 释放簇链 | 同上 |
| `rmdir(dir, name)` | 检查空 + 删除目录 | 同上 |
| `readdir(dir, &offset, &dirent)` | 遍历目录项 | VFS 已加 file->slk |
| `getattr(inode, &stat)` | 填写大小/时间戳等 | 无需锁 |
| `truncate(inode, size)` | 修改文件大小 | VFS 已校验 + 加 meta_lock |
| `destroy(inode)` | 释放 private + inode 本身 | refcount=0 时调用 |

### 3. file_operations（文件系统层必须实现）

| 接口 | 职责 | 并发 |
|------|------|------|
| `read(file, buf, count)` | 从磁盘读，返回字节数 | VFS 已加 file->slk |
| `write(file, buf, count)` | 写磁盘，返回字节数 | 同上 |
| `seek(file, offset, whence)` | 计算新偏移，VFS 更新 pos | 同上 |
| `close(file)` | 释放文件系统私有资源 | refcount=0 时调用 |

### 4. 文件系统层的"三不"原则

1. **不做权限校验**：权限由 VFS 层在 `vfs_permission()` 中统一拦截。文件系统层只需在 `inode->mode` 中填写 `S_IFREG` 或 `S_IFDIR`（文件类型），rwx 权限位由 VFS 层覆盖。
2. **不做并发互斥**：文件系统层认为自己并发安全。所有多进程并发写防护由 VFS 层的 `vfs_meta_lock`（sleeplock）统一完成，序列化所有元数据修改操作。
3. **不管理 inode 生命周期**：refcount 由 VFS 层管理。文件系统层创建的 inode refcount=1（归 caller），destroy 在 refcount=0 时由 VFS 层调用。

## 四、VFS 层的三大职责

### 1. 路径规范化

`vfs_normalize()` 在所有路径 API 入口统一处理：
- 相对路径拼接 cwd
- 消解 `.`、`..`、`//`
- 输出绝对路径，长度 ≤ VFS_MAX_PATH_LEN

原因：FAT32 等文件系统的目录中没有 `.`/`..` 目录项，必须在进入 inode walk 前消解。

### 2. 权限校验（VFS 层拦截）

| 操作 | 权限要求 | 实现位置 |
|------|---------|---------|
| 路径遍历每级 | MAY_EXEC | `vfs_lookup_impl` |
| open(O_RDONLY) | MAY_READ | `vfs_open` |
| open(O_WRONLY) | MAY_WRITE | `vfs_open` |
| open(O_RDWR) | MAY_READ \| MAY_WRITE | `vfs_open` |
| create | MAY_WRITE \| MAY_EXEC (父目录) | `vfs_create` |
| mkdir | MAY_WRITE \| MAY_EXEC (父目录) | `vfs_mkdir` |
| unlink | MAY_WRITE \| MAY_EXEC (父目录) | `vfs_unlink` |
| rmdir | MAY_WRITE \| MAY_EXEC (父目录) | `vfs_rmdir` |
| truncate | MAY_WRITE | `vfs_truncate` |

文件系统层**不需要**也不应该做权限校验。文件系统层在 `inode->mode` 中只填写文件类型（S_IFREG/S_IFDIR），权限位由 VFS 层在 create/mkdir 返回后覆盖：
```c
new_inode->mode = (new_inode->mode & S_IFMT) | mode;
new_inode->uid  = task->cred.uid;
new_inode->gid  = task->cred.gid;
```

### 3. 并发保护

| 锁类型 | 保护范围 | 保护对象 |
|--------|---------|---------|
| `vfs_meta_lock` (sleeplock) | 元数据修改操作 | create/mkdir/unlink/rmdir/truncate |
| `file->slk` (sleeplock) | 同一 file 对象的读写 | read/write/seek/readdir |
| `icache.lock` (spinlock) | inode cache hashmap | cache_find/insert/remove |
| `inode->lock` (spinlock) | inode refcount | inode_get/inode_put |
| `mount_table.lock` (spinlock) | 挂载树 | mount/umount/find_mount |

**文件系统层无需持有任何锁**——VFS 层在调用 iops/fops 前已完成所有锁的获取。

## 五、引用计数生命周期

严格限制inode引用计数是为了防止引用泄露

```
inode refcount = cache引用 + caller引用 + file引用 + cwd引用 + sb引用

创建场景:
  fs->lookup() → refcount=1 (caller)
  → inode_cache_insert → refcount=2 (cache+caller)
  → caller 用完 inode_put → refcount=1 (cache)
  → cache_remove → refcount=0 → destroy

open 场景:
  vfs_lookup → refcount=1 (caller)
  → file->inode = inode (引用转移, 不是新增)
  → 成功: 不需要 inode_get (lookup引用变为file引用)
  → 失败: inode_put (释放lookup引用)

cwd 场景:
  set_cwd → inode_get(new) + inode_put(old)
```

**原则**：文件系统层返回的 inode refcount=1（归 caller），VFS 层负责所有后续的 get/put。文件系统层的 destroy 只在 refcount=0 时被 VFS 调用。

## 六、新文件系统接入注意事项

以下条目来自真实调试（ext2 接入时的挂载失败案例），接入任何新文件系统时都应逐条检查。

### 1. 每个返回给 VFS 的 inode 都必须带正确的文件类型位

「三不原则」里"只填 `S_IFREG` / `S_IFDIR`，权限位由 VFS 覆盖"只适用于 **create / mkdir 新建路径**——VFS 在 `vfs_create()` / `vfs_mkdir()` 返回后会执行：

```c
new_inode->mode = (new_inode->mode & S_IFMT) | mode;
```

以下两条路径 VFS **不会**补任何 mode 位，文件系统层必须自己保证 `inode->mode` 高 4 位（`S_IFMT`）正确：

- `lookup()` 返回的**已存在** inode（`vfs_lookup_impl` 直接把 fs 返回的 inode 插入 cache，不动 mode）
- `get_super()` 构造的 **root inode**（根路径 `/` 的 walk 直接取 `sb->root`，根本不经过 lookup）

也就是说，从磁盘读出 inode 时，`inode->mode` 必须原样取自磁盘元数据（如 ext2 的 `i_mode`），而不是只写类型位或留 0。root inode 类型位错误的典型症状：

```
set_cwd("/") → vfs_lookup("/") → sb->root
             → (inode->mode & S_IFMT) != S_IFDIR → -1
             → init_user panic
```

挂载阶段建议自检：`get_super()` 读到 root 后立即断言 `(root->mode & S_IFMT) == S_IFDIR`，可以第一时间暴露磁盘解析错误。

### 2. 磁盘布局中的"版本相关 / 可变长度"字段必须从超级块严格解析，禁止硬编码

磁盘上每条 inode / 目录项记录的大小若随文件系统版本或格式化参数变化，**必须**以超级块字段为准做定位计算，不能用一个"常见默认值"硬编码。

典型案例（bug 1/2）：ext2 的磁盘 inode 大小

| 版本 | inode 大小来源 |
|------|---------------|
| rev 0 (`s_rev_level == 0`) | 固定 128 |
| rev 1 | 超级块偏移 88 的 `s_inode_size`，现代 `mkfs.ext2`（e2fsprogs 1.47）**即使 1K 块也默认写 256** |

inode 表定位公式：

```c
group     = (ino - 1) / s_inodes_per_group;
idx       = (ino - 1) % s_inodes_per_group;
blk_off   = (idx * inode_size) / block_size;
byte_off  = (idx * inode_size) % block_size;
```

`inode_size` 一旦取错（如实际 256 却按 128 算），组内**所有 idx>0 的 inode 定位全部错位**。root inode 号恒为 2（idx=1），首当其冲：按 128 算出的偏移 +128 落在它 256 字节记录的中段，于是 `i_mode` 读成 0 或垃圾值——表象就是第 1 条的"root 不是目录"。

通用检查清单：

- 超级块结构里变长字段（ext2 `s_inode_size`、FAT 扇区/簇参数等）是否按真实偏移解析，而不是被 `padding` 占位略过；
- 取值后做合法性校验（范围、2 的幂、不超过 block_size），非法直接挂载失败；
- 用 `mkfs.xxx` 默认参数（而非手工老镜像）做验证，现代工具默认参数往往就是非默认的那档。

### 3. 「Not a directory」类报错的排查顺序

当挂载后路径解析报"不是目录"时，**先查挂载根对象，再查 lookup**：

1. `get_super()` 是否成功读到超级块（magic、版本、块大小）；
2. `sb->root` 的 `mode / size / private` 是否与磁盘 root inode 一致（重点：类型位、定位参数）；
3. root 能识别后，再查 `lookup()` 的目录项解析与 inode 表定位。

不要一上来怀疑路径规范化或 VFS 权限层——根路径 `/` 不经过它们。

排查时优先使用文件系统官方工具交叉验证（ext2 用 `debugfs -R 'stat <2>'`、`dumpe2fs -h`），
拿它的输出与自己代码逐字段、逐偏移对表，比肉眼看磁盘 hex 快且不易数错。

### 4. 磁盘结构定义要按偏移逐个核对；漏掉一个字段，后面全部错位

新文件系统的 on-disk struct 建议显式写出每个字段的字节偏移注释，并用 `__attribute__((packed))`；超级块里"不打算使用"的 rev1+ 字段也必须按规范长度占位。字段错位**不会有任何编译告警**，只会在运行时以莫名其妙的方式暴露。

典型案例（bug 2/2）：ext2 inode 漏掉 `i_blocks`

ext2 inode 在偏移 28 处有一个 **`i_blocks`（文件占用的 512 字节扇区数）**，之后才是 `i_flags`(32)、`i_osd1`(36)、`i_block[15]`(40)。实现时把它漏写，`i_block[]` 就整体左移 4 字节：

| 磁盘偏移 | 规范字段 | 漏字段时代码读到的 |
|---------|---------|------------------|
| 28 | `i_blocks`（root=8，一个 4K 块=8 扇区） | 被当成 `i_flags` |
| 32 | `i_flags`（=0） | 被当成 `i_osd1` |
| 36 | `i_osd1`（=0） | 被当成 `i_block[0]` ← **恒为 0（hole）** |
| 40 | `i_block[0]`（root 目录块） | 被当成 `i_block[1]` |

注意它和 bug 1 的关系：修好 `s_inode_size` 后 `i_mode` 正确、S_IFDIR 检查通过，但**数据访问仍然全错**——root 目录的 `i_block[0]` 读成 0，readdir/lookup 看到的是空目录，`/init` 永远找不到。两个 bug 必须一起修。

两条防御措施：

```c
/* 1. 固定大小的核心 on-disk 结构加编译期尺寸断言 */
_Static_assert(sizeof(struct ext2_inode) == 128,
               "ext2 inode base size must be 128 bytes");

/* 2. get_super 阶段对 root 做关键字段断言，尽早暴露解析错误 */
if ((root->mode & S_IFMT) != S_IFDIR) { kill_sb(...); return -1; }
```

经验法则：on-disk 结构里凡是"不打算用"的字段，也要在结构中占位并标注偏移；写完结构先数一遍总长度是否与规范一致，再去写逻辑。

