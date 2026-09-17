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