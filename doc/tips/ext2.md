# ext2 实现踩Trap总结与优化技巧

本文件记录在 loeux-riscv 内核中实现最精简 ext2 文件系统过程中遇到的真实Trap、
根因分析与解决方法，以及后续性能优化，作为再次接入新文件系统时的提示。

涉及代码：
- [fs/ext2.c](file:///home/loe/work/os/loeux-riscv/fs/ext2.c)
- [include/fs/ext2.h](file:///home/loe/work/os/loeux-riscv/include/fs/ext2.h)
- [doc/fs/ext2.md](file:///home/loe/work/os/loeux-riscv/doc/fs/ext2.md)
- [doc/paradigm/vfs.md](file:///home/loe/work/os/loeux-riscv/doc/paradigm/vfs.md)

---

## Trap 1：inode 大小硬编码为 128，导致 root 被判为"非目录"

### 现象

挂载 ext2 后，`init_user → set_cwd("/")` 在
[kernel/proc.c](file:///home/loe/work/os/loeux-riscv/kernel/proc.c) 的
`(inode->mode & S_IFMT) != S_IFDIR` 检查处失败，返回 -1，init 进程跑不起来。

### 根因

`mke2fs` 1.47.2（e2fsprogs）默认生成 **rev1 + `s_inode_size=256`**，
即使块大小是 1024 也是 256（用 `dumpe2fs -h` 可证实）。
原代码把磁盘 inode 大小硬编码为 128，inode 表定位公式：

```
inode_block = bg_inode_table + (idx * inode_size) / block_size
byte_off    = (idx * inode_size) % block_size
```

`inode_size` 取错（实际 256 却按 128 算），组内所有 idx>0 的 inode 定位全部错位。
root inode 号恒为 2（idx=1），首当其冲：按 128 算出的偏移 +128 落在它 256 字节记录的
中段（`i_block` 区域），于是 `i_mode` 读成 0 或垃圾值——表象就是"root 不是目录"。

### 解决

- 超级块结构按 rev1 真实偏移解析：`s_first_ino`(84)、`s_inode_size`(88)、
  `s_block_group_nr`(90)、三个 feature 字段、`s_rev1_rest[36]`。
- `ext2_get_super` 中：rev0 或 `s_inode_size==0` → 128，否则取 `s_inode_size`，
  并校验 `[128, block_size]` 且为 2 的幂，非法直接挂载失败。
- 挂载时增加 root 自检：`(root->mode & S_IFMT) != S_IFDIR` 则挂载失败，
  在 `get_super` 阶段第一时间暴露磁盘解析错误。

### 注意

- VFS 只在 create/mkdir 路径覆盖 mode；lookup 与 get_super(root) 路径
  **必须由 fs 层自己保证 S_IFMT 类型位正确**。
- 不要用"手工老镜像"做验证，现代 `mkfs` 默认参数往往就是非默认的那档；
  优先用 `debugfs -R 'stat <2>'`、`dumpe2fs -h` 与代码逐偏移对表。

---

## Trap 2：`struct ext2_inode` 漏掉偏移 28 的 `i_blocks`，后续字段全错位

### 现象

修好Trap 1 后，`i_mode` 正确、S_IFDIR 检查通过，但**数据访问仍然全错**：
root 目录的 `i_block[0]` 读成 0（hole），readdir 看到空目录，`/init` 永远找不到。

### 根因

ext2 inode 在偏移 28 处有一个 **`i_blocks`（文件占用的 512 字节扇区数）**，
之后才是 `i_flags`(32)、`i_osd1`(36)、`i_block[15]`(40)。
实现时把它漏写，`i_block[]` 整体左移 4 字节：

| 磁盘偏移 | 规范字段 | 漏字段时代码读到的 |
|---------|---------|------------------|
| 28 | `i_blocks`（root=8，一个 4K 块=8 扇区） | 被当成 `i_flags` |
| 32 | `i_flags`（=0） | 被当成 `i_osd1` |
| 36 | `i_osd1`（=0） | 被当成 `i_block[0]` ← **恒为 0（hole）** |
| 40 | `i_block[0]`（root 目录块） | 被当成 `i_block[1]` |

字段错位**不会有任何编译告警**，只会在运行时以莫名其妙的方式暴露。
关键认知：**Trap 1 和Trap 2 必须一起修**——只修Trap 1 会让 `i_mode` 正确假象掩盖数据访问仍错。

### 解决

- `struct ext2_inode` 插入 `uint32_t i_blocks`（偏移 28），逐字段标注偏移。
- 加编译期尺寸断言，错位会在编译期立即暴露：

```c
_Static_assert(sizeof(struct ext2_inode) == 128,
               "ext2 inode base size must be 128 bytes");
```

- `ext2_inode_priv` 增加 `i_blocks`，在 `ext2_disk_to_priv` / `ext2_priv_to_disk`
  中往返保存（否则 `ext2_priv_to_disk` 里 `memset(out,0,...)` 会把它清零）。
- 新增 `ext2_refresh_i_blocks()`：在 write / truncate / mkdir / create 路径
  按实际已分配块重算扇区计数（非 hole 计数 × `block_size/512`），与 e2fsck 口径一致。

### 注意

- on-disk 结构里凡是"不打算用"的字段，也要在结构中占位并标注偏移。
- 写完结构先数一遍总长度是否与规范一致，再去写逻辑。
- `i_blocks` 是 **512 字节扇区数**，不是逻辑块数；2K 块对应 4，4K 块对应 8。

---

## 优化：write-back 块缓存（buffer cache）

### 问题

无缓存时，`ext2_read_block` / `ext2_write_block` 每次都同步走 virtio，
一个 4K 块要 8 次 512B 扇区往返。一次 `mkdir` 会反复读写同一批元数据块
（inode bitmap、block bitmap、inode 表块、父目录块），叠加起来就是明显卡顿。

经估算，一次 mkdir（4K 块、父目录 1 块）无缓存约 144 次 sector IO；
加缓存后读放大基本消除、写合并，可降到 ~70 次。

### 方案

在 `struct ext2_fs_priv` 上挂一张 hashmap（`utils/hashmap`），
键 = 块号 `uint32_t`，值 = `ext2_cache_entry{block, data[block_size], dirty}`：

- `ext2_read_block`：命中则 `memcpy` 返回；未命中走 raw 读入缓存再 `memcpy`。
- `ext2_write_block`：只写缓存并标记 `dirty`，不立即落盘。
- `ext2_cache_flush`：在 `mkdir`/`create`/`unlink`/`rmdir`/`truncate`/`write`
  等顶层修改操作**成功返回前**一次性把所有 dirty 块写回磁盘。
- 只读路径（`lookup`/`readdir`）不写盘，纯读缓存，命中即返回。

### 关键陷阱

1. **持自旋锁做磁盘 IO → `sched: noff != 1` panic（mkdir 实测触发）**：
   最初的 flush 在 `hash_table_foreach` 回调里直接 `ext2_disk_write_raw`。
   `hash_table_foreach` 回调全程持有 `ht->lock` 自旋锁（持锁 = push_off，
   中断关闭）；而 virtio 同步 IO `virtio_disk_rw_sync` 在轮询完成前会**主动
   `intr_on()` 重新开中断**，轮询期间被时钟中断抢占 → `yield → sched()`，
   此时 noff=2（ht->lock 一次 + 进程锁一次），sched 断言
   `noff != 1` 直接 panic。

   修法是 **gather-then-write 两段式**：
   - foreach 回调内只把 dirty 条目指针收集到数组并清 dirty，**绝不做 IO**；
   - foreach 返回（锁释放）后再逐块 `ext2_disk_write_raw`；写失败重新置 dirty。
   - 同理，"未命中读盘"必须在 `hash_table_lookup` 返回之后（锁已释放）才读盘。
   - 条目多时按 slab 单次上限（4096B = 512 个指针）分批；一批全失败则终止，
     防设备故障死循环。

2. **hashmap key 生命周期**：`hash_table_insert` 只保存 key 指针不复制。
   因此 key 必须是持久分配的——用 `&entry->block`（entry 在 slab 上），
   **不能**用栈上临时变量。`hash_table_lookup` 则可用栈上 key（它不保存）。

3. **insert 冲突**：`hash_table_insert(overwrite=true)` 不返回旧 value，
   直接覆盖会泄漏旧条目。改用"先 `lookup` 判断，存在就更新，不存在才
   `hash_table_insert_if_absent`"两步，避免泄漏。

4. **错误回滚路径**：`get_super` 里 cache 初始化后，后续失败路径
   （`sb_obj` 分配失败、`ext2_inode_read` 失败、root 非目录等）都要调
   `ext2_cache_destroy` 释放 hashmap 结构与已缓存条目，否则泄漏。

5. **退化保正确**：cache 初始化失败或 slab 分配失败时，
   `ext2_read_block`/`ext2_write_block` 自动退化到 raw 直读直写，
   保证功能正确（只是慢），不会 panic。

### 生命周期

- `get_super`：`priv` 字段设好后、第一次 `ext2_inode_read` 前调 `ext2_cache_init`。
- `kill_sb`：先 `ext2_cache_flush`（落盘 dirty）再 `ext2_cache_destroy`（释放）。
- 各修改性操作成功末尾调 `ext2_cache_flush`。

---

## 通用经验

1. **磁盘结构按偏移逐个核对**，写完先数总长度；固定大小的核心结构加
   `_Static_assert`，错位会在编译期而非运行时暴露。
2. **变长字段（inode size 等）从超级块解析**，取值后做合法性校验，
   非法直接挂载失败。
3. **用文件系统官方工具交叉验证**（`debugfs`、`dumpe2fs`），比肉眼看磁盘 hex 快
   且不易数错；拿它的输出与自己代码逐字段、逐偏移对表。
4. **"Not a directory"先查挂载根对象再查 lookup**：`get_super` → root 的
   mode/size/private → lookup 的目录项解析与 inode 表定位。根路径 `/` 不经过
   路径规范化或权限层，别一上来怀疑它们。
5. **块设备以 512 字节扇区为单位**，`ext2_read_block` 需循环 `block_size/512` 次；
   块缓存能把这些扇区往返合并为一次块级访问。
6. **自旋锁临界区内绝不能做可能阻塞/被抢占的事**（磁盘 IO、sleep、变长内存
   分配等待）。本内核 virtio 同步 IO 在轮询前会主动 `intr_on()`，持锁调用
   会在时钟抢占时以 `sched: noff != 1` 暴露。通用模式：锁内收集/修改状态，
   锁外做 IO。`noff != N` 类 panic 优先排查"持锁路径上是否多了一次 push_off
   或 raw intr_off/on 不配对"。
