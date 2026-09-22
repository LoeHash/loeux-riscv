#include <gpu/virtio_gpu.h>
#include <test/test.h>
#include <drivers/virtio_mmio.h>
#include <mm/dma.h>
#include <mm/memlayout.h>
#include <mm/vm.h>
#include <mm/slab.h>
#include <kernel/panic.h>
#include <kernel/printk.h>
#include <kernel/spinlock.h>
#include <lib.h>

// 字段核对
_Static_assert(sizeof(struct virtio_gpu_ctrl_hdr) == 24,
	       "virtio_gpu_ctrl_hdr must be 24 bytes");
_Static_assert(sizeof(struct virtio_gpu_resource_create_2d) == 40,
	       "virtio_gpu_resource_create_2d must be 40 bytes");
_Static_assert(sizeof(struct virtio_gpu_mem_entry) == 16,
	       "virtio_gpu_mem_entry must be 16 bytes");
_Static_assert(sizeof(struct virtio_gpu_resource_attach_backing) == 32,
	       "virtio_gpu_resource_attach_backing must be 32 bytes");
_Static_assert(sizeof(struct virtio_gpu_set_scanout) == 48,
	       "virtio_gpu_set_scanout must be 48 bytes");
_Static_assert(sizeof(struct virtio_gpu_resource_flush) == 48,
	       "virtio_gpu_resource_flush must be 48 bytes");
_Static_assert(sizeof(struct virtio_gpu_transfer_to_host_2d) == 56,
	       "virtio_gpu_transfer_to_host_2d must be 56 bytes");

static struct virtio_gpu_device root_gpu_device = {0};
static struct virtio_gpu_device* gpu_get_last();

/*
 * flush 热路径的命令/响应缓冲。
 * T2D 和 FLUSH 各用独立缓冲，virtqueue_send_dual 可同时提交，
 * 一次 NOTIFY 处理两条命令，QEMU 同步往返减半。
 */
static struct virtio_gpu_transfer_to_host_2d flush_t2d
    __attribute__((aligned(16)));
static struct virtio_gpu_resource_flush flush_fl __attribute__((aligned(16)));
static struct virtio_gpu_set_scanout flush_ss __attribute__((aligned(16)));
static struct virtio_gpu_ctrl_hdr flush_resp1 __attribute__((aligned(16)));
static struct virtio_gpu_ctrl_hdr flush_resp2 __attribute__((aligned(16)));
static spinlock_t gpu_flush_lock = {0};

static void dump_gpu(struct virtio_gpu_device* gpu);
static void detect_gpu_device();
static int virtio_gpu_init(struct virtio_gpu_device* gpu);
static void init_all_gpu_device();
static void init_all_gpu();
static void virtio_gpu_handshake(uintptr_t base);
static int virtio_gpu_negotiate_features(uintptr_t base);
static int virtio_gpu_setup_controlq(struct virtio_gpu_device* gpu);
static int virtio_gpu_setup_cursorq(struct virtio_gpu_device* gpu);
static void virtio_gpu_driver_ok(uintptr_t base);
static int virtio_gpu_get_display_info(struct virtio_gpu_device* gpu);
static int virtio_gpu_create_framebuffer(struct virtio_gpu_device* gpu);
static volatile uint32_t gpu_device_count = 0;

struct virtio_gpu_device* gpu_get(uint32_t idx)
{
	struct virtio_gpu_device* now = root_gpu_device.next;

	for (uint32_t i = 0; i < idx; i++) {
		now = now->next;
	}

	return now;
}

void init_virtio_gpu()
{
	init_spinlock(&gpu_flush_lock);
	detect_gpu_device();
	init_all_gpu();
}

static void init_all_gpu(void)
{
	struct virtio_gpu_device* now = root_gpu_device.next;

	while (now != NULL) {
		int ret = virtio_gpu_init(now);
		if (ret != 0 || now->initialized != 1)
			panic_error("init_all_gpu: gpu init failed!\n");

		now = now->next;
	}
}

static int virtio_gpu_init(struct virtio_gpu_device* gpu)
{
	uintptr_t base = gpu->mmio_base;
	int ret;

	virtio_gpu_handshake(base);

	if ((ret = virtio_gpu_negotiate_features(base))) {
		printk("[Wrong] can not negotiate to the gpu!\n");
		return ret;
	}

	if ((ret = virtio_gpu_setup_controlq(gpu))) {
		printk("[Wrong] can not setup the gpu's controlq!\n");
		return ret;
	}

	if ((ret = virtio_gpu_setup_cursorq(gpu))) {
		printk("[Wrong] can not setup the gpu's cursorq!\n");
		return ret;
	}

	virtio_gpu_driver_ok(base);

	if ((ret = virtio_gpu_get_display_info(gpu))) {
		printk("[Wrong] can not get the display info!\n");
		return ret;
	}

	if ((ret = virtio_gpu_create_framebuffer(gpu))) {
		printk("[Wrong] can not create the framebuffer\n");
		return ret;
	}

	gpu->initialized = 1;

	dump_gpu(gpu);

	// kgfx_test_all(gpu);

	return 0;
}

static void detect_gpu_device()
{
	for (int i = 0; i < VIRTIO_MMIO_COUNT; i++) {
		if (*R_LEVEL(VIRTIO_MMIO_MAGIC_VALUE_OFFSET, i) !=
		    VIRTIO_MMIO_MAGIC) {
			continue;
		}

		if (*R_LEVEL(VIRTIO_MMIO_DEVICE_ID_OFFSET, i) !=
		    VIRTIO_GPU_DEVICE_ID) {
			// empty.
			continue;
		}

		if (*R_LEVEL(VIRTIO_MMIO_VERSION_OFFSET, i) != 2) {
			// nope, not supported.
			continue;
		}

		// we got a new gpu device
		struct virtio_gpu_device* new_gpu =
		    slab_alloc(sizeof(struct virtio_gpu_device));

		if (new_gpu == NULL) {
			panic_error("detect_gpu_device(): Failed to allocate "
				    "gpu device\n");
		}

		memset(new_gpu, 0, sizeof(struct virtio_gpu_device));
		new_gpu->next = NULL;
		new_gpu->mmio_base = GET_VIR_BASE(i);
		new_gpu->initialized = 0;

		// mount to the last gpu
		gpu_get_last()->next = new_gpu;
		gpu_device_count++;
	}
}

static struct virtio_gpu_device* gpu_get_last()
{
	struct virtio_gpu_device* tmp = &root_gpu_device;
	struct virtio_gpu_device* next = root_gpu_device.next;

	while ((next != NULL)) {
		tmp = next;
		next = tmp->next;
	}
	return tmp;
}

static void virtio_gpu_handshake(uintptr_t base)
{
	// status 清零
	b32_write(base + VIRTIO_MMIO_STATUS_OFFSET, 0);
	// 确认
	b32_write(base + VIRTIO_MMIO_STATUS_OFFSET, VIRTIO_STATUS_ACKNOWLEDGE);
	// 确认，开始驱动当前gpu
	uint32_t status = b32_read(base + VIRTIO_MMIO_STATUS_OFFSET);
	b32_write(base + VIRTIO_MMIO_STATUS_OFFSET,
		  status | VIRTIO_STATUS_DRIVER);
}

static int virtio_gpu_negotiate_features(uintptr_t base)
{
	uint32_t feat_lo, feat_hi;

	// low 32
	b32_write(base + VIRTIO_MMIO_DEVICE_FEATURES_SEL_OFFSET, 0);
	feat_lo = b32_read(base + VIRTIO_MMIO_DEVICE_FEATURES_OFFSET);

	// high 32
	b32_write(base + VIRTIO_MMIO_DEVICE_FEATURES_SEL_OFFSET, 1);
	feat_hi = b32_read(base + VIRTIO_MMIO_DEVICE_FEATURES_OFFSET);

	// 我们不需要任何特性
	// 只需要基础的
	b32_write(base + VIRTIO_MMIO_DRIVER_FEATURES_SEL_OFFSET, 0);
	b32_write(base + VIRTIO_MMIO_DRIVER_FEATURES_OFFSET, 0);
	b32_write(base + VIRTIO_MMIO_DRIVER_FEATURES_SEL_OFFSET, 1);
	b32_write(base + VIRTIO_MMIO_DRIVER_FEATURES_OFFSET, 1);

	// 回写，提示设备
	uint32_t status = b32_read(base + VIRTIO_MMIO_STATUS_OFFSET);
	b32_write(base + VIRTIO_MMIO_STATUS_OFFSET,
		  status | VIRTIO_STATUS_FEATURES_OK);

	// for ensuring...
	if (!(b32_read(base + VIRTIO_MMIO_STATUS_OFFSET) &
	      VIRTIO_STATUS_FEATURES_OK))
		return -1;

	return 0;
}

static int virtio_gpu_setup_controlq(struct virtio_gpu_device* gpu)
{

	uintptr_t base = gpu->mmio_base;
	int ret;

	// 设置 queue 0
	b32_write(base + VIRTIO_MMIO_QUEUE_SEL_OFFSET, 0);

	// 读取queue max
	// uint32_t max_queue_size = b32_read(base +
	// VIRTIO_MMIO_QUEUE_NUM_MAX_OFFSET); max_queue_size = max_queue_size >
	// 64 ? 64 : max_queue_size;

	// 设置max
	b32_write(base + VIRTIO_MMIO_QUEUE_NUM_OFFSET,
		  VIRTIO_GPU_MAX_QUEUE_NUM);

	// 创建vq命令队列
	struct virtqueue_n* gpu_vq = alloc_virtqueue(VIRTIO_GPU_MAX_QUEUE_NUM);
	if ((ret = init_virtqueue(gpu_vq, 0))) {
		panic_error(
		    "virtio_gpu_setup_controlq(): can not create gpu vq!\n");
	}
	gpu_vq->mmio_base = base;
	gpu_vq->queue_idx = 0;
	gpu_vq->queue_size = VIRTIO_GPU_MAX_QUEUE_NUM;
	gpu->controlq = gpu_vq;

	// 设置设备回写物理地址
	uint64_t desc_phy = gpu->controlq->desc_phy;
	uint64_t avail_phy = gpu->controlq->avail_phy;
	uint64_t used_phy = gpu->controlq->used_phy;

	b32_write(base + VIRTIO_MMIO_QUEUE_DESC_LOW_OFFSET,
		  GET_LOW_32(desc_phy));
	b32_write(base + VIRTIO_MMIO_QUEUE_DESC_HIGH_OFFSET,
		  GET_HIGH_32(desc_phy));

	b32_write(base + VIRTIO_MMIO_QUEUE_AVAIL_LOW_OFFSET,
		  GET_LOW_32(avail_phy));
	b32_write(base + VIRTIO_MMIO_QUEUE_AVAIL_HIGH_OFFSET,
		  GET_HIGH_32(avail_phy));

	b32_write(base + VIRTIO_MMIO_QUEUE_USED_LOW_OFFSET,
		  GET_LOW_32(used_phy));
	b32_write(base + VIRTIO_MMIO_QUEUE_USED_HIGH_OFFSET,
		  GET_HIGH_32(used_phy));

	// 激活
	b32_write(base + VIRTIO_MMIO_QUEUE_READY_OFFSET, 1);

	return 0;
}

static int virtio_gpu_setup_cursorq(struct virtio_gpu_device* gpu)
{
	uintptr_t base = gpu->mmio_base;
	int ret;

	// 设置 queue 1
	b32_write(base + VIRTIO_MMIO_QUEUE_SEL_OFFSET, 1);

	// 读取queue max
	// uint32_t max_queue_size = b32_read(base +
	// VIRTIO_MMIO_QUEUE_NUM_MAX_OFFSET); max_queue_size = max_queue_size >
	// 64 ? 64 : max_queue_size;

	// 设置max
	b32_write(base + VIRTIO_MMIO_QUEUE_NUM_OFFSET,
		  VIRTIO_GPU_MAX_QUEUE_NUM);

	// 创建vq命令队列
	struct virtqueue_n* gpu_vq = alloc_virtqueue(VIRTIO_GPU_MAX_QUEUE_NUM);
	if ((ret = init_virtqueue(gpu_vq, 0))) {
		panic_error(
		    "virtio_gpu_setup_controlq(): can not create gpu vq!\n");
	}
	gpu_vq->mmio_base = base;
	gpu_vq->queue_idx = 1;
	gpu_vq->queue_size = VIRTIO_GPU_MAX_QUEUE_NUM;
	gpu->cursorq = gpu_vq;

	// 设置设备回写物理地址
	uint64_t desc_phy = gpu->cursorq->desc_phy;
	uint64_t avail_phy = gpu->cursorq->avail_phy;
	uint64_t used_phy = gpu->cursorq->used_phy;

	b32_write(base + VIRTIO_MMIO_QUEUE_DESC_LOW_OFFSET,
		  GET_LOW_32(desc_phy));
	b32_write(base + VIRTIO_MMIO_QUEUE_DESC_HIGH_OFFSET,
		  GET_HIGH_32(desc_phy));

	b32_write(base + VIRTIO_MMIO_QUEUE_AVAIL_LOW_OFFSET,
		  GET_LOW_32(avail_phy));
	b32_write(base + VIRTIO_MMIO_QUEUE_AVAIL_HIGH_OFFSET,
		  GET_HIGH_32(avail_phy));

	b32_write(base + VIRTIO_MMIO_QUEUE_USED_LOW_OFFSET,
		  GET_LOW_32(used_phy));
	b32_write(base + VIRTIO_MMIO_QUEUE_USED_HIGH_OFFSET,
		  GET_HIGH_32(used_phy));

	// 激活
	b32_write(base + VIRTIO_MMIO_QUEUE_READY_OFFSET, 1);

	return 0;
}

static void virtio_gpu_driver_ok(uintptr_t base)
{
	uint32_t status = b32_read(base + VIRTIO_MMIO_STATUS_OFFSET);
	b32_write(base + VIRTIO_MMIO_STATUS_OFFSET,
		  status | VIRTIO_STATUS_DRIVER_OK);
}

static int virtio_gpu_get_display_info(struct virtio_gpu_device* gpu)
{

	// 分配命令缓冲区
	struct virtio_gpu_ctrl_hdr* cmd =
	    slab_alloc(sizeof(struct virtio_gpu_ctrl_hdr));
	if (!cmd)
		return -1;

	// 分配响应缓冲区
	struct virtio_gpu_resp_display_info* resp =
	    slab_alloc(sizeof(struct virtio_gpu_resp_display_info));
	if (!resp) {
		slab_free(cmd);
		return -1;
	}

	// 填命令头
	cmd->type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO; // 0x0100
	cmd->flags = 0;
	cmd->fence_id = 0;
	cmd->ctx_id = 0;
	cmd->padding = 0;

	// printk("QUEUE_NUM macro=%d\n", VIRTIO_GPU_MAX_QUEUE_NUM);
	// printk("desc_phy=%0#lx avail_phy=%0#lx used_phy=%0#lx\n",
	// gpu->controlq->desc_phy, gpu->controlq->avail_phy,
	// gpu->controlq->used_phy); printk("cmd=%0#lx resp=%0#lx\n",
	// (uint64_t)cmd, (uint64_t)resp);

	uintptr_t base = gpu->mmio_base;
	b32_write(base + VIRTIO_MMIO_QUEUE_SEL_OFFSET, 0); // 选中 controlq

	/*
	 * 注意：QUEUE_NUM / QUEUE_DESC_* / QUEUE_AVAIL_* / QUEUE_USED_*
	 */
	printk("=== controlq state ===\n");
	/* read-only: device-advertised MAX depth (controlq=64, cursorq=16) */
	printk("  queue_num_max = %u\n",
	       b32_read(base + VIRTIO_MMIO_QUEUE_NUM_MAX_OFFSET));
	/* read-write: 1 means queue activated; QUEUE_NUM itself is write-only
	 */
	printk("  queue_ready   = %u (1=active)\n",
	       b32_read(base + VIRTIO_MMIO_QUEUE_READY_OFFSET));
	printk("  desc_phy  = %0#lx\n", gpu->controlq->desc_phy);
	printk("  avail_phy = %0#lx\n", gpu->controlq->avail_phy);
	printk("  used_phy  = %0#lx\n", gpu->controlq->used_phy);
	printk("=== ===\n");

	printk("notify_reg = %0#lx\n",
	       (uint64_t)(base + VIRTIO_MMIO_QUEUE_NOTIFY_OFFSET));

	// 发送并等待响应
	int ret = virtqueue_send(gpu->controlq,
				 cmd,
				 sizeof(struct virtio_gpu_ctrl_hdr),
				 resp,
				 sizeof(struct virtio_gpu_resp_display_info));

	// 检查响应类型
	if (ret == 0 && resp->hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO)
		ret = -1;

	// 解析分辨率：找第一个 enabled 的 scanout
	if (ret == 0) {
		for (int i = 0; i < 16; i++) {
			if (resp->pmodes[i].enabled) {
				gpu->width = resp->pmodes[i].r.width;
				gpu->height = resp->pmodes[i].r.height;
				gpu->scanout_id = i;
				break;
			}
		}
	}

	// 释放
	slab_free(cmd);
	slab_free(resp);

	return ret;
}

static int virtio_gpu_create_framebuffer(struct virtio_gpu_device* gpu)
{
	int ret;

	// 1. DMA 分配
	/*
	 * backing 高度取屏幕的 2 倍：多出的一屏是滚屏 panning 的余量。
	 * 滚一行时 SET_SCANOUT 窗口在 resource 内下移 16px，只需 T2D
	 * 新露出的底部行；窗口移到底后才做一次整屏搬运并归零，从而把
	 * 滚屏 DMA 从每帧 4MB（全屏）降到每帧 ~20KB（一个字符行）。
	 */
	gpu->fb_height = gpu->height * 2;
	gpu->fb_size = (uint64_t)gpu->width * gpu->fb_height * 4;
	gpu->fb = dma_alloc(gpu->fb_size);
	if (!gpu->fb) {
		printk("create_fb: dma_alloc failed, size=%lu\n", gpu->fb_size);
		return -1;
	}
	gpu->fb_phy = va2pa(kernel_pt, (uint64_t)gpu->fb);
	gpu->resource_id = 1;
	memset(gpu->fb, 0, gpu->fb_size);

	// printk("create_fb: fb=%0#lx phy=%0#lx size=%lu\n",
	//        (uint64_t)gpu->fb, gpu->fb_phy, gpu->fb_size);

	// 2. CREATE_2D
	{
		struct virtio_gpu_resource_create_2d* cmd =
		    slab_alloc(sizeof(*cmd));
		struct virtio_gpu_ctrl_hdr* resp = slab_alloc(sizeof(*resp));
		if (!cmd || !resp) {
			slab_free(cmd);
			slab_free(resp);
			return -1;
		}

		cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
		cmd->hdr.flags = 0;
		cmd->hdr.fence_id = 0;
		cmd->hdr.ctx_id = 0;
		cmd->hdr.padding = 0;
		cmd->resource_id = gpu->resource_id;
		cmd->format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
		cmd->width = gpu->width;
		cmd->height = gpu->fb_height;

		ret = virtqueue_send(
		    gpu->controlq, cmd, sizeof(*cmd), resp, sizeof(*resp));

		// printk("create_2d: ret=%d type=%0#x\n", ret, resp->type);

		if (ret == 0 && resp->type != VIRTIO_GPU_RESP_OK_NODATA)
			ret = -1;

		slab_free(cmd);
		slab_free(resp);
		if (ret)
			return ret;
	}

	// 3. ATTACH_BACKING
	{
		struct {
			struct virtio_gpu_resource_attach_backing req;
			struct virtio_gpu_mem_entry entry;
		} __attribute__((packed)) * cmd;

		struct virtio_gpu_ctrl_hdr* resp;

		cmd = slab_alloc(sizeof(*cmd));
		resp = slab_alloc(sizeof(*resp));
		if (!cmd || !resp) {
			slab_free(cmd);
			slab_free(resp);
			return -1;
		}

		cmd->req.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
		cmd->req.hdr.flags = 0;
		cmd->req.hdr.fence_id = 0;
		cmd->req.hdr.ctx_id = 0;
		cmd->req.hdr.padding = 0;
		cmd->req.resource_id = gpu->resource_id;
		cmd->req.nr_entries = 1;

		cmd->entry.addr = gpu->fb_phy;
		cmd->entry.length = gpu->fb_size;
		cmd->entry.padding = 0;

		ret = virtqueue_send(
		    gpu->controlq, cmd, sizeof(*cmd), resp, sizeof(*resp));

		// printk("attach: ret=%d type=%0#x\n", ret, resp->type);

		if (ret == 0 && resp->type != VIRTIO_GPU_RESP_OK_NODATA)
			ret = -1;

		slab_free(cmd);
		slab_free(resp);
		if (ret)
			return ret;
	}

	// 4. SET_SCANOUT
	{
		struct virtio_gpu_set_scanout* cmd = slab_alloc(sizeof(*cmd));
		struct virtio_gpu_ctrl_hdr* resp = slab_alloc(sizeof(*resp));
		if (!cmd || !resp) {
			slab_free(cmd);
			slab_free(resp);
			return -1;
		}

		cmd->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
		cmd->hdr.flags = 0;
		cmd->hdr.fence_id = 0;
		cmd->hdr.ctx_id = 0;
		cmd->hdr.padding = 0;
		cmd->r.x = 0;
		cmd->r.y = 0;
		cmd->r.width = gpu->width;
		cmd->r.height = gpu->height;
		cmd->scanout_id = gpu->scanout_id;
		cmd->resource_id = gpu->resource_id;

		ret = virtqueue_send(
		    gpu->controlq, cmd, sizeof(*cmd), resp, sizeof(*resp));

		// printk("set_scanout: ret=%d type=%0#x\n", ret, resp->type);

		if (ret == 0 && resp->type != VIRTIO_GPU_RESP_OK_NODATA)
			ret = -1;

		slab_free(cmd);
		slab_free(resp);
		if (ret)
			return ret;
	}

	return 0;
}

static void dump_gpu(struct virtio_gpu_device* gpu)
{
	if (gpu == NULL) {
		printk("gpu: (null)\n");
		return;
	}

	printk("============== VirtIO GPU ==============\n");
	printk("  mmio_base   : %0#lx\n", gpu->mmio_base);
	printk("  controlq    : %0#lx\n", (uint64_t)gpu->controlq);
	printk("  cursorq     : %0#lx\n", (uint64_t)gpu->cursorq);
	printk("  resource_id : %u\n", gpu->resource_id);
	printk("  width       : %u\n", gpu->width);
	printk("  height      : %u\n", gpu->height);
	printk("  fb          : %0#lx\n", (uint64_t)gpu->fb);
	printk("  fb_phy      : %0#lx\n", gpu->fb_phy);
	printk("  fb_size     : %lu\n", gpu->fb_size);
	printk("  scanout_id  : %u\n", gpu->scanout_id);
	printk("  initialized : %d\n", gpu->initialized);
}

/* 发一条无数据响应的控制命令；成功 0 失败 -1 */
static int gpu_send_ctrl(struct virtio_gpu_device* gpu,
			 void* cmd,
			 uint32_t cmd_len,
			 struct virtio_gpu_ctrl_hdr* resp)
{
	memset(resp, 0, sizeof(*resp));
	if (virtqueue_send(gpu->controlq, cmd, cmd_len, resp, sizeof(*resp)) !=
	    0)
		return -1;
	return resp->type == VIRTIO_GPU_RESP_OK_NODATA ? 0 : -1;
}

/*
 * 上屏一帧脏矩形（坐标为 resource 绝对坐标）。
 *   rescan=0（常态）：T2D + FLUSH 用 virtqueue_send_dual 一次 NOTIFY 提交，
 *                    QEMU 同步处理往返从 2 次减为 1 次。
 *   rescan=1（panning 滚屏）：T2D 先单独提交，SET_SCANOUT + FLUSH 再
 *                    一次 NOTIFY 批量提交（3 次→2 次往返）。
 */
int virtio_gpu_present(struct virtio_gpu_device* gpu,
		       uint32_t x,
		       uint32_t y,
		       uint32_t w,
		       uint32_t h,
		       int rescan,
		       uint32_t scanout_y)
{
	int ret;

	if (w == 0 || h == 0)
		return 0;

	acquire(&gpu_flush_lock);

	uint64_t offset = ((uint64_t)y * gpu->width + x) * 4;

	/* 构造 T2D */
	memset(&flush_t2d, 0, sizeof(flush_t2d));
	flush_t2d.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
	flush_t2d.r.x = x;
	flush_t2d.r.y = y;
	flush_t2d.r.width = w;
	flush_t2d.r.height = h;
	flush_t2d.offset = offset;
	flush_t2d.resource_id = gpu->resource_id;

	/* 构造 FLUSH */
	memset(&flush_fl, 0, sizeof(flush_fl));
	flush_fl.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
	flush_fl.r.x = x;
	flush_fl.r.y = y;
	flush_fl.r.width = w;
	flush_fl.r.height = h;
	flush_fl.resource_id = gpu->resource_id;

	if (rescan) {
		/* panning：先提交 T2D */
		ret = gpu_send_ctrl(
		    gpu, &flush_t2d, sizeof(flush_t2d), &flush_resp1);
		if (ret) {
			release(&gpu_flush_lock);
			return ret;
		}

		/* SET_SCANOUT */
		memset(&flush_ss, 0, sizeof(flush_ss));
		flush_ss.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
		flush_ss.r.x = 0;
		flush_ss.r.y = scanout_y;
		flush_ss.r.width = gpu->width;
		flush_ss.r.height = gpu->height;
		flush_ss.scanout_id = gpu->scanout_id;
		flush_ss.resource_id = gpu->resource_id;

		/* SET_SCANOUT + FLUSH 批量提交 */
		ret = virtqueue_send_dual(gpu->controlq,
					  &flush_ss,
					  sizeof(flush_ss),
					  &flush_resp1,
					  sizeof(flush_resp1),
					  &flush_fl,
					  sizeof(flush_fl),
					  &flush_resp2,
					  sizeof(flush_resp2));
		if (ret) {
			release(&gpu_flush_lock);
			return ret;
		}
		ret = (flush_resp1.type == VIRTIO_GPU_RESP_OK_NODATA &&
		       flush_resp2.type == VIRTIO_GPU_RESP_OK_NODATA)
			  ? 0
			  : -1;
	} else {
		/* 常态：T2D + FLUSH 一次 NOTIFY */
		ret = virtqueue_send_dual(gpu->controlq,
					  &flush_t2d,
					  sizeof(flush_t2d),
					  &flush_resp1,
					  sizeof(flush_resp1),
					  &flush_fl,
					  sizeof(flush_fl),
					  &flush_resp2,
					  sizeof(flush_resp2));
		if (ret) {
			release(&gpu_flush_lock);
			return ret;
		}
		ret = (flush_resp1.type == VIRTIO_GPU_RESP_OK_NODATA &&
		       flush_resp2.type == VIRTIO_GPU_RESP_OK_NODATA)
			  ? 0
			  : -1;
	}

	release(&gpu_flush_lock);
	return ret;
}

int virtio_gpu_flush(struct virtio_gpu_device* gpu,
		     uint32_t x,
		     uint32_t y,
		     uint32_t w,
		     uint32_t h)
{
	/* 无 panning 的普通刷新（test 等路径） */
	return virtio_gpu_present(gpu, x, y, w, h, 0, 0);
}

void gpu_update(struct virtio_gpu_device* gpu,
		uint32_t x,
		uint32_t y,
		uint32_t w,
		uint32_t h)
{
	virtio_gpu_flush(gpu, x, y, w, h);
}
