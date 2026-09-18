好，照着 VFS 文档的风格，写一份 **TTY 层开发范式**。

---

# TTY 层开发范式

## 一、TTY 层概览

| 功能 | 状态 | 说明 |
|------|------|------|
| tty 注册 | **Active** | 注册为 char_device，路径 `/dev/tty/<name>` |
| 行规则（line discipline） | **Active** | 行缓冲、回显、退格、回车成行 |
| 读写接口 | **Active** | `read` 走行规则，`write` 直通底层 |
| 底层适配 | **Active** | `putc` / `getc` 两个函数对接具体设备 |
| 阻塞等待 | **Active** | 底层 `getc` 自行实现阻塞语义 |
| 多 tty 实例 | **Active** | 每个 tty 一个 `struct tty` 对象，独立缓冲与锁 |

## 二、设计思想

```
┌──────────────────────────────────────────────────────┐
│                    TTY Layer                          │
│  ┌─────────────┐  ┌──────────────┐  ┌────────────┐   │
│  │ 行规则       │  │ 回显控制      │  │ 锁与缓冲    │   │
│  │line_discipline│ │echo          │  │read_lock   │   │
│  └─────────────┘  └──────────────┘  └────────────┘   │
│  ┌─────────────────────────────────────┐             │
│  │ char_device 适配层 (tty_cdev_ops)    │             │
│  │ open/read/write/close                │             │
│  └─────────────────────────────────────┘             │
├──────────────────────────────────────────────────────┤
│              Bottom Driver Layer (xxx_tty.c)           │
│  ┌─────────────────────────────────────────────┐      │
│  │ 只负责原始字符收发                            │      │
│  │ 实现 putc（输出一个字符）                     │      │
│  │ 实现 getc（阻塞读一个字符）                   │      │
│  │ 不处理行编辑、不回显、不判断字符语义           │      │
│  └─────────────────────────────────────────────┘      │
├──────────────────────────────────────────────────────┤
│              Hardware / Firmware Layer                 │
│  UART / virtio-input / virtio-gpu / framebuffer        │
└──────────────────────────────────────────────────────┘
```

## 三、接入新终端的范式

要接入一个新终端，只需实现以下接口并注册：

### 1. TTY 描述符

```c
static const struct tty_ops my_tty_ops = {
    .open  = my_tty_open,    // 可空
    .close = my_tty_close,   // 可空
    .putc  = my_tty_putc,    // 必须实现
    .getc  = my_tty_getc,    // 必须实现
};

static struct tty my0_tty;

void init_my_tty(void)
{
    my0_tty.name = "my/0";
    my0_tty.ops  = &my_tty_ops;
    my0_tty.priv = NULL;

    my0_tty.linepos    = 0;
    my0_tty.line_ready = 0;
    init_spinlock(&my0_tty.read_lock);
    init_spinlock(&my0_tty.output_lock);
    my0_tty.read_chan = &my0_tty.read_chan;

    tty_register(&my0_tty);
}
```

通过 `tty_register(&my0_tty)` 注册，用户态路径为 `/dev/tty/my/0`。

### 2. tty_ops（底层驱动必须实现）

| 接口 | 职责 | 约束 |
|------|------|------|
| `putc(tty, c)` | 输出一个字符到底层设备 | 不阻塞（或允许短阻塞），不睡眠 |
| `getc(tty, out)` | 阻塞读一个原始字符 | 必须阻塞直到有字符；返回 0 成功，-1 失败 |
| `open(tty, flags)` | 打开终端 | 可空；只在 open 时调用一次 |
| `close(tty)` | 关闭终端 | 可空；只在 close 时调用 |

**核心只有 `putc` 和 `getc`。** 其余两个可空。

### 3. 底层驱动的"三不"原则

1. **不做行编辑**：行缓冲、退格删字符、回车成行，全由 TTY 层统一处理。底层只返回原始字符。
2. **不做回显**：回显由 TTY 层在行规则中调用 `putc` 完成。底层 `getc` 只管读，不管显示。
3. **不判断字符语义**：`\r`、`\n`、`\b`、`0x7f` 等控制字符的处理由 TTY 层负责。底层原样返回。

## 四、TTY 层的三大职责

### 1. 行规则（line discipline）

`tty_line_discipline(tty, c)` 在 TTY 层统一处理每个原始字符：

| 输入字符 | 行缓冲动作 | 回显动作 |
|---------|-----------|---------|
| 普通字符 | 追加到 `linebuf` | 原样输出 |
| `\r` / `\n` | 追加 `\n`，置 `line_ready` | 输出 `\r\n` |
| `\b` / `0x7f` | `linepos--`（若 >0） | 输出 `\b \b` |
| 其他控制字符 | 暂不处理| 不回显 |

行规则是**纯函数式**的：输入一个字符，更新状态，决定回显内容，返回“是否成行”。

### 2. 读写接口

| 接口 | 职责 | 并发保护 |
|------|------|---------|
| `tty_cdev_read` | 攒行 → 拷贝给用户 | `read_lock` 保护行缓冲 |
| `tty_cdev_write` | 逐字符调 `putc` | `output_lock` 保护输出不交叉 |

**读**：检查 `line_ready`，未就绪则调底层 `getc` 拿原始字符，交给行规则处理，循环直到成行。

**写**：逐字符调用 `putc`，不加工内容（换行符处理由上层负责，或由底层负责）。

### 3. 锁与并发

| 锁类型 | 保护范围 | 保护对象 |
|--------|---------|---------|
| `read_lock` (spinlock) | 行缓冲 | `linebuf` / `linepos` / `line_ready` |
| `output_lock` (spinlock) | 输出 | 防止多进程输出字符交叉 |

**底层驱动无需持有任何 TTY 层的锁。** TTY 层在调用 `putc` / `getc` 前后完成所有锁的获取和释放。

**注意**：`getc` 调用期间不持有 `read_lock`，因为 `getc` 可能长时间阻塞。`putc` 调用期间不持有 `read_lock`，避免锁顺序问题。

## 五、字符语义规范

### 输入侧

| 字符 | 十六进制 | 语义 | 行缓冲 | 回显 |
|------|---------|------|--------|------|
| 可打印 | 0x20-0x7e | 普通字符 | 追加 | 原样 |
| `\r` | 0x0d | 回车 | 追加 `\n`，成行 | `\r\n` |
| `\n` | 0x0a | 换行 | 追加 `\n`，成行 | `\r\n` |
| `\b` | 0x08 | 退格 | `linepos--` | `\b \b` |
| DEL | 0x7f | 退格 | `linepos--` | `\b \b` |

### 输出侧

| 字符 | 处理 |
|------|------|
| `\n` | 是否补 `\r` 由底层决定，TTY 层不强制 |
| 其他 | 原样输出 |

## 六、新终端接入注意事项

### 1. `getc` 必须阻塞

TTY 层的 `tty_cdev_read` 调 `getc` 时期望它“一直等到有字符才返回”。底层怎么实现阻塞由底层决定：

- **uart_tty**：调 SBI 的 `sbi_getchar()`，SBI 本身阻塞。
- **console_tty**：用 `sleep_locked` 等输入队列。

**如果 `getc` 不阻塞（立即返回 0 或 -1），`tty_cdev_read` 会空转或错误退出。**

### 2. `name` 必须生命周期覆盖整个内核运行

`tty_register` 内部把 `name` 拼接成 `"tty/<name>"` 并注册。`vfs_register_chardev` 存的是指针，不复制内容。所以 `tty->name` 必须是**静态字符串**，不能是栈上的临时数组。

### 3. 一个 tty 对象只能注册一次

`tty_register` 内部用静态数组 `name_storage` 存注册名，最多 16 个。超过会返回 -1。每个 tty 对象只应注册一次，重复注册会失败。

### 4. 锁顺序

- `tty_cdev_read`：拿 `read_lock` → 改缓冲 → 释放 → 调 `getc` → 拿 `read_lock` → 改缓冲 → 释放 → 调 `putc`。
- `tty_cdev_write`：拿 `output_lock` → 循环调 `putc` → 释放。
- **不得在持有 `read_lock` 时调 `getc`**，因为 `getc` 可能长时间阻塞。

### 5. 回显路径

回显由 TTY 层调用 `putc` 完成。**底层 `putc` 不区分“回显”和“程序输出”，都走同一个路径。** 这就是为什么回显和程序输出在终端上表现一致。

### 6. 多读者问题

当前 TTY 层**不支持多个读者同时 `read` 同一个 tty**。如果两个进程同时 `read`，它们会争抢 `getc` 的字符，行缓冲会错乱。

**如需多读者，需要引入输入队列和 `sleep_locked` 机制**，把“字符到达”和“行成行”分开。当前实现是单读者模型。

## 七、实现检查清单

接入新终端时逐条检查：

- [ ] `putc` 实现：输出一个字符，不睡眠。
- [ ] `getc` 实现：阻塞读一个字符，返回 0/-1。
- [ ] `name` 是静态字符串。
- [ ] `linepos = 0`，`line_ready = 0`。
- [ ] `init_spinlock(&read_lock)`。
- [ ] `init_spinlock(&output_lock)`。
- [ ] `read_chan = &read_chan`。
- [ ] 调 `tty_register()`，检查返回值。
- [ ] 启动流程里调 `init_xxx_tty()`。
- [ ] 用户态 open `/dev/tty/xxx/0` 验证。

---

**!!! Warning !!!**：底层实现 `putc` 和 `getc`，填 `tty_ops`，调 `tty_register`。行规则、回显、锁，全在 TTY 层。