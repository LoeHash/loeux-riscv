# GPU TTY 性能优化

> 当前: loeux-riscv 1280×800 virtio-gpu
> 问题：终端打字回显、滚屏明显卡顿，而同样在 QEMU 上跑 Linux 终端很流畅。

---

## 一、现象

1. **打字回显延迟大**：按下键到字符上屏，肉眼可感的迟滞。
2. **滚屏卡顿**：长输出（`cat` / `ls` 大目录）时画面一卡一卡地跳，不是连续滚动。
3. **对比强烈**：同一 QEMU、同一分辨率，Linux 终端很快，自己的内核很卡。

## 二、根因分析

经过 QEMU trace `-trace 'enable=virtio_gpu*'`、QMP 注键时序、screendump 截图逐行比对，定位到 **4 个层次的瓶颈**，按影响从大到小：

### 瓶颈 1：每字符 6 次 GPU 命令往返（第一轮已修）

`gpu_tty_putc` 每输出一个字符要做：擦光标 → 画字符 → 画光标，每一步都调一次 `kgfx_update`，
而 `kgfx_update` 直接发 `TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH`。

**后果**：QEMU 每个 `RESOURCE_FLUSH` 都触发一次 SDL 窗口重绘。打一个字符 = 6 次全屏重绘请求。
长输出时每秒几十个字符 = 上百次窗口重绘，QEMU 端的渲染线程成为瓶颈。

**对比 Linux**：Linux fbcon / DRM 把终端输出攒成帧，每帧只 flush 一次。

### 瓶颈 2：系统节拍只有 10Hz（100ms）

QEMU virt 的 mtime 时钟是 10MHz（`0x989680`），中断间隔误用了
`BASE_FREQUENCY / TASK_CPU_SLIP_FACTOR`，而 `TASK_CPU_SLIP_FACTOR = 10`，
导致 **每秒只有 10 次时钟中断（100ms 间隔）**。

键盘 `keyboard_getchar` 在 `wfi` 中等待，只能被时钟中断唤醒。
即使按键事件已到达，最坏要等近 100ms 才被调度处理。

**证据**：`ext2.c` / `fat32.c` 的时间戳用 `ticks / 100`，
原本是 100Hz 也就是每 tick = 10ms，但实际跑在 10Hz。

### 瓶颈 3：滚屏每行全屏 4MB DMA

`kgfx_scroll_up` 用 `memmove` 把整块 1280×800（4MB）上移一行，
然后标脏整个屏幕，`kgfx_flush_now` 再把这 4MB 通过 `TRANSFER_TO_HOST_2D` DMA 给 QEMU。

**后果**：滚 50 行（一屏）= 50 次 4MB 搬运 = 200MB 内存拷贝 + 50 次窗口重绘。
`cat` 一个长文件，guest 内核在疯狂搬显存，QEMU 在疯狂重绘窗口。

**这是 panning 优化要消灭的核心瓶颈。**

### 瓶颈 4：memmove / memset / memcpy 按字节循环

`lib.h` 中的内存操作是逐字节循环。4MB 搬运 = 400 万次 TCG 迭代。
RISC-V TCG 是软件模拟，每条 `lb`/`sb` 指令都要走翻译缓存，
按字节的循环比 64 位宽拷贝慢一个数量级。

## 三、解决方案

### 3.1 节拍 10Hz → 100Hz

```
include/kernel/timer.h   新增 #define TIMER_TICKS_PER_SEC 100
kernel/start.c            主核/从核 2 处 中断间隔改用该宏
kernel/trap.c             2 处 同上
```

调度器每个 tick 都 yield（无时间片计数），改频率安全。
按键唤醒延迟从 ~100ms 降到 ~10ms。

### 3.2 延迟合并刷新（脏矩形 + 节流）

核心思想：**把"每个字符立即 flush"改成"攒一批脏区域，定时统一 flush"**。

```
kgfx_update(gpu)       → no-op，仅由绘图函数内部 mark_dirty 累积脏矩形
kgfx_timer_tick()      → 100Hz 时钟驱动，节流到 50fps（每 2 tick 刷一次）
                          多核竞争由 dirty_lock 内 take_dirty 原子取走来保证
kgfx_flush_now()       → 强制立即刷（打字回显等需要即时上屏的场景）
```

**脏矩形合并**：多个字符的绘制区域 union 成一个包围盒，一次 T2D+FLUSH 传完。

**节流**：100Hz 下每 2 tick（20ms）最多刷一次，避免高频输出时 flush 风暴。

### 3.3 宽字内存操作

```
include/lib.h  memmove  重写：8 字节对齐时 uint64 宽拷贝，支持前向/反向重叠
               memset   重写：头部补对齐 + uint64 模式填充 (val * 0x0101...)
               memcpy   重写：uint64 宽拷贝
```

4MB 搬运从 400 万次字节迭代降到 50 万次 64 位迭代。

### 3.4 Panning 环形滚动（核心优化）

**这是本轮最重要的改动，彻底消灭了滚屏的全屏搬运。**

#### 原理

virtio-gpu 的 2D resource 是设备侧的一块 backing 镜像。
`SET_SCANOUT` 命令把屏幕（scanout）映射到 resource 内的一个矩形窗口。
如果把 resource 做成屏幕高度的 **2 倍**，屏幕只是其中一个高度为 H 的窗口：

```
  Resource (backing, fb_height = 2*H)
  ┌────────────────────┐
  │  y=0               │
  │  ...  旧内容       │ ← 窗口从这里开始时，显示这段
  │  y=H-1             │
  │  y=H  ───────────  │ ← 当前 scanout 窗口 [pan_y, pan_y+H)
  │  ...  新内容       │
  │  y=2H-1            │
  └────────────────────┘
```

滚一行时：
1. **不搬像素**：只把 `pan_y += line_h`，发一条 `SET_SCANOUT` 平移窗口。
2. 旧行本来就在 resource 里，无需重传。
3. 只需传输新露出的底部条带（1280×16 ≈ 80KB），而非整屏 4MB。
4. 窗口移到 backing 底部后，做一次 4MB `memmove` 回绕 + 全屏重传并归零——
   每滚 50 行（一屏）才发生一次。

#### DMA 量对比

| 场景        | 优化前（每行）  | 优化后（每行）  | 倍数   |
| ----------- | -------------- | -------------- | ------ |
| 常规滚一行  | 4MB T2D        | 80KB T2D       | **50×** |
| 回绕帧      | 4MB T2D        | 4MB T2D        | 1×     |
| 回绕频率    | 每行           | 每 50 行       | **50×** |

#### 代码结构

```
include/drivers/gpu/virtio_gpu.h
  struct 新增 fb_height（=2*height）
  声明 virtio_gpu_present(gpu, x, y, w, h, rescan, scanout_y)

drivers/gpu/virtio_gpu.c
  create_framebuffer:  fb_height = height*2, fb_size = width*fb_height*4
                        CREATE_2D 的 height 用 fb_height
                        ATTACH_BACKING 用 fb_size
                        初始 SET_SCANOUT 窗口在 y=0
  virtio_gpu_present:  T2D(脏矩形) → 可选 SET_SCANOUT(窗口平移) → FLUSH(脏矩形)
  virtio_gpu_flush:    薄封装 → present(..., rescan=0, scanout_y=0)

drivers/gpu/kgfx.c
  static pan_y, pan_changed
  所有绘图函数：屏幕 y → resource 绝对 y（+pan_y），接口不变
  kgfx_scroll_up:  pan_y += line_h；常规帧只脏底部条带；回绕帧 memmove + 全屏重传
  kgfx_clear:      复位 pan_y=0，清整个 backing
  kgfx_flush_now:  take_frame 取出脏矩形 + rescan + scanout_y，调 virtio_gpu_present
  kgfx_timer_tick: 节流 50fps 调 flush_now
```

#### 坐标系约定

- **绘图接口**（`draw_char` / `fill_rect` / `put_pixel`）接收**屏幕坐标** (0..W, 0..H)。
- **写 fb 与标脏**一律用 **resource 绝对坐标** = 屏幕坐标 + `pan_y`。
- `pan_y` 仅在 `dirty_lock` 内被改写，绘制路径直接读（32 位对齐读写本身原子）。

#### 回绕帧的脏区处理细节

回绕时（`memmove` 把 `[H, 2H)` 搬到 `[0, H)`）：
- 旧脏区（如擦光标留下的 y≈1590 的标记）落在 backing 高位，已失效。
- 不能用 `mark_dirty_locked` 做 union——否则会和旧脏区 union 出覆盖 `2H` 的巨型矩形，平白多传 4MB。
- 必须**直接重置**脏区为单屏 `(0,0,W,H)`。

#### 竞争安全

- 所有内存改动 + pan 更新 + 标脏都在 `dirty_lock` 内完成，避免定时器中断在"已标脏但像素还没写完"的窗口里刷走半帧。
- `kgfx_take_frame` 在锁内原子取出脏区+pan 状态并清零，多核竞争时同一帧只有一个 hart 真正发命令。

### 3.5 TTY 层调整

```
drivers/tty/tty.c        write 末尾的强制 flush 删除（输出走 100Hz 节拍合并）
                         read 在即将阻塞前调 flush（保证回显即时）
drivers/tty/gpu_tty.c    flush 改调 kgfx_flush_now()
                         init_gpu_tty 清屏前调 kgfx_attach(gpu)，清屏后 flush_now
```

## 四、验证方法

### 4.1 QEMU trace

```bash
qemu-system-riscv64 ... \
  -trace 'enable=virtio_gpu*' -D /tmp/gpu_trace.log
```

- panning 成功标志：trace 里只见小条带（`w=1280, h=16`）的 T2D+FLUSH 和周期性 SET_SCANOUT。
- 全屏 4MB T2D 只在开机清屏和每 50 行回绕时出现。
- 优化前：90 行滚屏产生 53 次全屏 4MB flush。
- 优化后：100 行滚屏只产生 2 次全屏 flush（开机 + 1 次回绕）。

### 4.2 QMP 注键 + 截图

```python
# QEMU 10.2 的 input-send-event 格式
{"type":"key","data":{"down":True,"key":{"type":"qcode","data":"ret"}}}
```

注入 100 个回车强制滚屏，再 `screendump` 截图，逐行比对像素：
50 行 `#` 提示符全部对齐，历史保留不错位 → panning + 回绕内容正确。

### 4.3 零错误

QEMU stderr 为空，无 `Guest says index N is available` 等 virtqueue 错误。

## 五、优化效果汇总

| 指标                  | 优化前          | 优化后          |
| --------------------- | -------------- | -------------- |
| 时钟节拍               | 10Hz (100ms)   | 100Hz (10ms)   |
| 按键→上屏延迟          | ~100ms         | ~10ms          |
| 每字符 GPU 命令往返    | 6 次           | 0（攒批合并）   |
| 滚屏每行 DMA           | 4MB（全屏）    | 80KB（条带）   |
| 滚屏每行窗口重绘       | 1 次（全屏）   | 1 次（条带）   |
| 回绕全屏搬运频率       | 每行           | 每 50 行       |
| memmove 字宽           | 1 字节         | 8 字节 (uint64) |
| flush 节流             | 无             | 50fps          |

## 六、后续优化方向

1. **双缓冲 / vsync 对齐**：目前 flush 与 QEMU 窗口重绘没有同步，极端高频输出可能看到撕裂。可以在 flush 前读 QEMU 的 `VIRTIO_GPU_CMD_GET_DISPLAY_INFO` 或用 fence 同步。

2. **slab / dma_alloc 容量验证**：fb 现为 8MB（原 4MB），需确认页分配器在 2048M 物理内存下长期稳定。如果未来增大分辨率（4K），fb 会到 32MB+，可能需要分段 attach_backing（多个 mem_entry）。

3. **光标闪烁独立路径**：目前光标闪烁走 tty putc 的擦/画逻辑，每次都标脏。可以改成 kgfx 内部维护光标层，定时器 tick 里独立刷光标条带，不与文字输出脏区合并。

4. **行回绕策略调优**：当前每滚一屏（50 行）回绕一次。可以改成更大的环形缓冲（3-4 屏），减少回绕频率，代价是更多 backing 内存。或用 "copy on wrap" 策略延迟回绕直到真的需要。

5. **T2D 坐标裁剪**：当前脏矩形是所有绘制区域的 union，可能包含大片未实际变化的空白。可以改成多个独立脏矩形（最多 N 个），分别 T2D+FLUSH，减少无效传输。需要权衡命令数 vs 传输量。

6. **键盘 event buffer 扩容**：keyboard 只有 8 个 event buffer，QMP 瞬间注入大量按键时事件会被丢弃。真实 cat 突发在 guest 内部无此限制，但可考虑增大 buffer 或改用 ring buffer。

7. **SMP 负载均衡**：当前 4 核都会进时钟中断，kgfx flush 靠 dirty_lock 竞争保证只有一个核发命令。可以让 GPU 刷新绑定到固定 hart，减少锁竞争。

## 七、思想总结

### 7.1 立即刷新是万恶之源

终端 I/O 最直觉的做法是"输出一个字符就马上画到屏幕"。
这在裸机单片机上没问题，在有 GPU DMA + 窗口重绘的系统上是灾难：
每个字符触发一次跨设备往返，吞吐量被命令延迟拖垮。

**正确做法**：输出只写内存 + 标脏，刷新统一攒批 + 定时合并。
这本质就是 fbcon / DRM dirty tracking / Wayland frame callbacks 的思路。

### 7.2 搬运 & 指针

滚屏的本质是"让屏幕显示历史内容上方的新行"。
最直觉的做法是 `memmove` 搬像素——4MB 内存拷贝。
但 virtio-gpu 的 resource 是设备侧独立缓冲，`SET_SCANOUT` 可以零搬运地改窗口位置：
**用指针平移替代数据搬运**，DMA 量从 O(W×H) 降到 O(W×line_h)。
