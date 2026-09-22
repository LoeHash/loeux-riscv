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
static struct virtio_gpu_device *gpu_get_last();

/*
 * flush 热路径的命令/响应缓冲。
 * 用静态 BSS（内核恒等映射，物理地址 == 虚拟地址，与键盘 ev_bufs 同理），
 * 避免每次 flush 两次 slab 分配/释放；gpu_flush_lock 串行化使用。
 */
static union {
        struct virtio_gpu_transfer_to_host_2d t2d;
        struct virtio_gpu_resource_flush      fl;
        uint8_t                               pad[64];
} flush_cmd __attribute__((aligned(16)));
static struct virtio_gpu_ctrl_hdr flush_resp __attribute__((aligned(16)));
static spinlock_t gpu_flush_lock = {0};

static void dump_gpu(struct virtio_gpu_device *gpu);
static void detect_gpu_device();
static int virtio_gpu_init(struct virtio_gpu_device *gpu);
static void init_all_gpu_device();
static void init_all_gpu();
static void virtio_gpu_handshake(uintptr_t base);
static int virtio_gpu_negotiate_features(uintptr_t base);
static int virtio_gpu_setup_controlq(struct virtio_gpu_device *gpu);
static int virtio_gpu_setup_cursorq(struct virtio_gpu_device *gpu);
static void virtio_gpu_driver_ok(uintptr_t base);
static int virtio_gpu_get_display_info(struct virtio_gpu_device *gpu);
static int virtio_gpu_create_framebuffer(struct virtio_gpu_device *gpu);
static volatile uint32_t gpu_device_count = 0; 

struct virtio_gpu_device *gpu_get(uint32_t idx)
{
        struct virtio_gpu_device *now = root_gpu_device.next;

        for (uint32_t i = 0; i < idx; i++) {
                now = now->next;
        }

        return now;     
}


void init_virtio_gpu(){
        init_spinlock(&gpu_flush_lock);
        detect_gpu_device();
        init_all_gpu();
}

static void init_all_gpu(void)
{
        struct virtio_gpu_device *now = root_gpu_device.next;

        while (now != NULL) {
                int ret = virtio_gpu_init(now);
                if (ret != 0 || now->initialized != 1)
                        panic_error("init_all_gpu: gpu init failed!\n");

                now = now->next;
        }
}

static int virtio_gpu_init(struct virtio_gpu_device *gpu)
{
        uintptr_t base = gpu->mmio_base;
        int ret;

        virtio_gpu_handshake(base);

        if ((ret = virtio_gpu_negotiate_features(base))){
                printk("[Wrong] can not negotiate to the gpu!\n");
                return ret;
        }
        
        if ((ret = virtio_gpu_setup_controlq(gpu))){
                printk("[Wrong] can not setup the gpu's controlq!\n");
                return ret;
        }

        if ((ret = virtio_gpu_setup_cursorq(gpu))){
                printk("[Wrong] can not setup the gpu's cursorq!\n");
                return ret;
        }

        virtio_gpu_driver_ok(base);


        if ((ret = virtio_gpu_get_display_info(gpu))){
                printk("[Wrong] can not get the display info!\n");
                return ret;
        }


        if ((ret = virtio_gpu_create_framebuffer(gpu))){
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
                if (*R_LEVEL(VIRTIO_MMIO_MAGIC_VALUE_OFFSET, i) != VIRTIO_MMIO_MAGIC)
                {
                        continue;
                }

                if (*R_LEVEL(VIRTIO_MMIO_DEVICE_ID_OFFSET, i) != VIRTIO_GPU_DEVICE_ID)
                {
                        // empty.
                        continue;
                }

                if (*R_LEVEL(VIRTIO_MMIO_VERSION_OFFSET, i) != 2)
                {
                        // nope, not supported.
                        continue;
                }

                // we got a new gpu device
                struct virtio_gpu_device *new_gpu = slab_alloc(sizeof(struct virtio_gpu_device));

                if (new_gpu == NULL) {
                        panic_error("detect_gpu_device(): Failed to allocate gpu device\n");
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

static struct virtio_gpu_device *gpu_get_last(){
        struct virtio_gpu_device *tmp = &root_gpu_device;
        struct virtio_gpu_device *next = root_gpu_device.next;

        while ((next != NULL)) {
               tmp = next;
               next = tmp->next; 
        }
        return  tmp;

}

static void virtio_gpu_handshake(uintptr_t base){
        // status 清零
        b32_write(base + VIRTIO_MMIO_STATUS_OFFSET, 0);
        // 确认
        b32_write(base + VIRTIO_MMIO_STATUS_OFFSET, VIRTIO_STATUS_ACKNOWLEDGE);
        // 确认，开始驱动当前gpu
        uint32_t status = b32_read(base + VIRTIO_MMIO_STATUS_OFFSET);
        b32_write(base + VIRTIO_MMIO_STATUS_OFFSET, status | VIRTIO_STATUS_DRIVER);
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
        b32_write(base + VIRTIO_MMIO_STATUS_OFFSET, status | VIRTIO_STATUS_FEATURES_OK);

        // for ensuring...
        if (!(b32_read(base + VIRTIO_MMIO_STATUS_OFFSET) & VIRTIO_STATUS_FEATURES_OK))
                return -1;

        return 0;
}

static int virtio_gpu_setup_controlq(struct virtio_gpu_device *gpu){
        
        uintptr_t base = gpu->mmio_base;
        int ret;

        // 设置 queue 0
        b32_write(base + VIRTIO_MMIO_QUEUE_SEL_OFFSET, 0);
        
        // 读取queue max
        // uint32_t max_queue_size = b32_read(base + VIRTIO_MMIO_QUEUE_NUM_MAX_OFFSET);
        // max_queue_size = max_queue_size > 64 ? 64 : max_queue_size; 
        
        // 设置max
        b32_write(base + VIRTIO_MMIO_QUEUE_NUM_OFFSET, VIRTIO_GPU_MAX_QUEUE_NUM);

        // 创建vq命令队列
        struct virtqueue_n *gpu_vq = alloc_virtqueue(VIRTIO_GPU_MAX_QUEUE_NUM);
        if ((ret = init_virtqueue(gpu_vq, 0))) {
                panic_error("virtio_gpu_setup_controlq(): can not create gpu vq!\n");
        }
        gpu_vq->mmio_base = base;
        gpu_vq->queue_idx = 0;
        gpu_vq->queue_size = VIRTIO_GPU_MAX_QUEUE_NUM;
        gpu->controlq = gpu_vq;

        // 设置设备回写物理地址
        uint64_t desc_phy  = gpu->controlq->desc_phy;
        uint64_t avail_phy = gpu->controlq->avail_phy;
        uint64_t used_phy  = gpu->controlq->used_phy;

        b32_write(base + VIRTIO_MMIO_QUEUE_DESC_LOW_OFFSET,  GET_LOW_32(desc_phy));
        b32_write(base + VIRTIO_MMIO_QUEUE_DESC_HIGH_OFFSET, GET_HIGH_32(desc_phy));

        b32_write(base + VIRTIO_MMIO_QUEUE_AVAIL_LOW_OFFSET,  GET_LOW_32(avail_phy));
        b32_write(base + VIRTIO_MMIO_QUEUE_AVAIL_HIGH_OFFSET, GET_HIGH_32(avail_phy));

        b32_write(base + VIRTIO_MMIO_QUEUE_USED_LOW_OFFSET,  GET_LOW_32(used_phy));
        b32_write(base + VIRTIO_MMIO_QUEUE_USED_HIGH_OFFSET, GET_HIGH_32(used_phy));

        // 激活
        b32_write(base + VIRTIO_MMIO_QUEUE_READY_OFFSET, 1);

        return 0;
}

static int virtio_gpu_setup_cursorq(struct virtio_gpu_device *gpu){
        uintptr_t base = gpu->mmio_base;
        int ret;

        // 设置 queue 1
        b32_write(base + VIRTIO_MMIO_QUEUE_SEL_OFFSET, 1);
        
        // 读取queue max
        // uint32_t max_queue_size = b32_read(base + VIRTIO_MMIO_QUEUE_NUM_MAX_OFFSET);
        // max_queue_size = max_queue_size > 64 ? 64 : max_queue_size; 
        
        // 设置max
        b32_write(base + VIRTIO_MMIO_QUEUE_NUM_OFFSET,VIRTIO_GPU_MAX_QUEUE_NUM);

        // 创建vq命令队列
        struct virtqueue_n *gpu_vq = alloc_virtqueue(VIRTIO_GPU_MAX_QUEUE_NUM);
        if ((ret = init_virtqueue(gpu_vq, 0))) {
                panic_error("virtio_gpu_setup_controlq(): can not create gpu vq!\n");
        }
        gpu_vq->mmio_base = base;
        gpu_vq->queue_idx = 1;
        gpu_vq->queue_size = VIRTIO_GPU_MAX_QUEUE_NUM;
        gpu->cursorq = gpu_vq;

        // 设置设备回写物理地址
        uint64_t desc_phy  = gpu->cursorq->desc_phy;
        uint64_t avail_phy = gpu->cursorq->avail_phy;
        uint64_t used_phy  = gpu->cursorq->used_phy;

        b32_write(base + VIRTIO_MMIO_QUEUE_DESC_LOW_OFFSET,  GET_LOW_32(desc_phy));
        b32_write(base + VIRTIO_MMIO_QUEUE_DESC_HIGH_OFFSET, GET_HIGH_32(desc_phy));

        b32_write(base + VIRTIO_MMIO_QUEUE_AVAIL_LOW_OFFSET,  GET_LOW_32(avail_phy));
        b32_write(base + VIRTIO_MMIO_QUEUE_AVAIL_HIGH_OFFSET, GET_HIGH_32(avail_phy));

        b32_write(base + VIRTIO_MMIO_QUEUE_USED_LOW_OFFSET,  GET_LOW_32(used_phy));
        b32_write(base + VIRTIO_MMIO_QUEUE_USED_HIGH_OFFSET, GET_HIGH_32(used_phy));

        // 激活
        b32_write(base + VIRTIO_MMIO_QUEUE_READY_OFFSET, 1);

        return 0;
}

static void virtio_gpu_driver_ok(uintptr_t base)
{
        uint32_t status = b32_read(base + VIRTIO_MMIO_STATUS_OFFSET);
        b32_write(base + VIRTIO_MMIO_STATUS_OFFSET, status | VIRTIO_STATUS_DRIVER_OK);
}

static int virtio_gpu_get_display_info(struct virtio_gpu_device *gpu)
{


        // 分配命令缓冲区
        struct virtio_gpu_ctrl_hdr *cmd = slab_alloc(sizeof(struct virtio_gpu_ctrl_hdr));
        if (!cmd)
                return -1;

        // 分配响应缓冲区
        struct virtio_gpu_resp_display_info *resp =
                slab_alloc(sizeof(struct virtio_gpu_resp_display_info));
        if (!resp) {
                slab_free(cmd);
                return -1;
        }

        // 填命令头
        cmd->type     = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;  // 0x0100
        cmd->flags    = 0;
        cmd->fence_id = 0;
        cmd->ctx_id   = 0;
        cmd->padding  = 0;

        
        // printk("QUEUE_NUM macro=%d\n", VIRTIO_GPU_MAX_QUEUE_NUM);
        // printk("desc_phy=%0#lx avail_phy=%0#lx used_phy=%0#lx\n",
        // gpu->controlq->desc_phy, gpu->controlq->avail_phy, gpu->controlq->used_phy);
        // printk("cmd=%0#lx resp=%0#lx\n", (uint64_t)cmd, (uint64_t)resp);

        uintptr_t base = gpu->mmio_base;
        b32_write(base + VIRTIO_MMIO_QUEUE_SEL_OFFSET, 0);   // 选中 controlq

        /*
         * 注意：QUEUE_NUM / QUEUE_DESC_* / QUEUE_AVAIL_* / QUEUE_USED_*
         */
        printk("=== controlq state ===\n");
        /* read-only: device-advertised MAX depth (controlq=64, cursorq=16) */
        printk("  queue_num_max = %u\n", b32_read(base + VIRTIO_MMIO_QUEUE_NUM_MAX_OFFSET));
        /* read-write: 1 means queue activated; QUEUE_NUM itself is write-only */
        printk("  queue_ready   = %u (1=active)\n",
               b32_read(base + VIRTIO_MMIO_QUEUE_READY_OFFSET));
        printk("  desc_phy  = %0#lx\n", gpu->controlq->desc_phy);
        printk("  avail_phy = %0#lx\n", gpu->controlq->avail_phy);
        printk("  used_phy  = %0#lx\n", gpu->controlq->used_phy);
        printk("=== ===\n");

        printk("notify_reg = %0#lx\n", (uint64_t)(base + VIRTIO_MMIO_QUEUE_NOTIFY_OFFSET));

        // 发送并等待响应
        int ret = virtqueue_send(gpu->controlq,
                                 cmd,  sizeof(struct virtio_gpu_ctrl_hdr),
                                 resp, sizeof(struct virtio_gpu_resp_display_info));

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

static int virtio_gpu_create_framebuffer(struct virtio_gpu_device *gpu)
{
        int ret;

        // 1. DMA 分配
        gpu->fb_size     = gpu->width * gpu->height * 4;
        gpu->fb          = dma_alloc(gpu->fb_size);
        if (!gpu->fb) {
                printk("create_fb: dma_alloc failed, size=%lu\n", gpu->fb_size);
                return -1;
        }
        gpu->fb_phy      = va2pa(kernel_pt, (uint64_t)gpu->fb);
        gpu->resource_id = 1;
        memset(gpu->fb, 0, gpu->fb_size);

        // printk("create_fb: fb=%0#lx phy=%0#lx size=%lu\n",
        //        (uint64_t)gpu->fb, gpu->fb_phy, gpu->fb_size);

        // 2. CREATE_2D
        {
                struct virtio_gpu_resource_create_2d *cmd  = slab_alloc(sizeof(*cmd));
                struct virtio_gpu_ctrl_hdr           *resp = slab_alloc(sizeof(*resp));
                if (!cmd || !resp) { slab_free(cmd); slab_free(resp); return -1; }

                cmd->hdr.type     = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
                cmd->hdr.flags    = 0;
                cmd->hdr.fence_id = 0;
                cmd->hdr.ctx_id   = 0;
                cmd->hdr.padding  = 0;
                cmd->resource_id  = gpu->resource_id;
                cmd->format       = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
                cmd->width        = gpu->width;
                cmd->height       = gpu->height;

                ret = virtqueue_send(gpu->controlq,
                                     cmd,  sizeof(*cmd),
                                     resp, sizeof(*resp));

                // printk("create_2d: ret=%d type=%0#x\n", ret, resp->type);

                if (ret == 0 && resp->type != VIRTIO_GPU_RESP_OK_NODATA)
                        ret = -1;

                slab_free(cmd);
                slab_free(resp);
                if (ret) return ret;
        }

        // 3. ATTACH_BACKING
        {
                struct {
                        struct virtio_gpu_resource_attach_backing req;
                        struct virtio_gpu_mem_entry                entry;
                } __attribute__((packed)) *cmd;

                struct virtio_gpu_ctrl_hdr *resp;

                cmd  = slab_alloc(sizeof(*cmd));
                resp = slab_alloc(sizeof(*resp));
                if (!cmd || !resp) { slab_free(cmd); slab_free(resp); return -1; }

                cmd->req.hdr.type     = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
                cmd->req.hdr.flags    = 0;
                cmd->req.hdr.fence_id = 0;
                cmd->req.hdr.ctx_id   = 0;
                cmd->req.hdr.padding  = 0;
                cmd->req.resource_id  = gpu->resource_id;
                cmd->req.nr_entries   = 1;

                cmd->entry.addr    = gpu->fb_phy;
                cmd->entry.length  = gpu->fb_size;
                cmd->entry.padding = 0;

                ret = virtqueue_send(gpu->controlq,
                                     cmd,  sizeof(*cmd),
                                     resp, sizeof(*resp));

                // printk("attach: ret=%d type=%0#x\n", ret, resp->type);

                if (ret == 0 && resp->type != VIRTIO_GPU_RESP_OK_NODATA)
                        ret = -1;

                slab_free(cmd);
                slab_free(resp);
                if (ret) return ret;
        }

        // 4. SET_SCANOUT
        {
                struct virtio_gpu_set_scanout *cmd  = slab_alloc(sizeof(*cmd));
                struct virtio_gpu_ctrl_hdr    *resp = slab_alloc(sizeof(*resp));
                if (!cmd || !resp) { slab_free(cmd); slab_free(resp); return -1; }

                cmd->hdr.type     = VIRTIO_GPU_CMD_SET_SCANOUT;
                cmd->hdr.flags    = 0;
                cmd->hdr.fence_id = 0;
                cmd->hdr.ctx_id   = 0;
                cmd->hdr.padding  = 0;
                cmd->r.x          = 0;
                cmd->r.y          = 0;
                cmd->r.width      = gpu->width;
                cmd->r.height     = gpu->height;
                cmd->scanout_id   = gpu->scanout_id;
                cmd->resource_id  = gpu->resource_id;

                ret = virtqueue_send(gpu->controlq,
                                     cmd,  sizeof(*cmd),
                                     resp, sizeof(*resp));

                // printk("set_scanout: ret=%d type=%0#x\n", ret, resp->type);

                if (ret == 0 && resp->type != VIRTIO_GPU_RESP_OK_NODATA)
                        ret = -1;

                slab_free(cmd);
                slab_free(resp);
                if (ret) return ret;
        }

        return 0;
}

static void dump_gpu(struct virtio_gpu_device *gpu)
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

/*
 * 把 fb 的一块脏区域刷到屏幕。
 * 现代qemu需要选转移，在flush
 *   1) TRANSFER_TO_HOST_2D：把 guest backing(fb) 的像素 DMA 到设备侧
 *      resource 镜像；QEMU 不会直接引用 backing。
 *   2) RESOURCE_FLUSH：通知 scanout 该区域已变化，重绘到窗口。
 */
int virtio_gpu_flush(struct virtio_gpu_device *gpu,
                     uint32_t x, uint32_t y,
                     uint32_t w, uint32_t h)
{
        int ret;

        if (w == 0 || h == 0)
                return 0;

        /* 命令缓冲是静态共享的，且同一 controlq 不允许并发提交 */
        acquire(&gpu_flush_lock);

        /* backing 内源数据的字节偏移：第 y 行第 x 个像素 */
        uint64_t offset = ((uint64_t)y * gpu->width + x) * 4;

        /* 1) TRANSFER_TO_HOST_2D */
        {
                struct virtio_gpu_transfer_to_host_2d *cmd  = &flush_cmd.t2d;
                struct virtio_gpu_ctrl_hdr            *resp = &flush_resp;

                memset(cmd, 0, sizeof(*cmd));
                memset(resp, 0, sizeof(*resp));

                cmd->hdr.type     = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
                cmd->r.x          = x;
                cmd->r.y          = y;
                cmd->r.width      = w;
                cmd->r.height     = h;
                cmd->offset       = offset;
                cmd->resource_id  = gpu->resource_id;

                ret = virtqueue_send(gpu->controlq,
                                     cmd,  sizeof(*cmd),
                                     resp, sizeof(*resp));

                if (ret == 0 && resp->type != VIRTIO_GPU_RESP_OK_NODATA)
                        ret = -1;

                if (ret) {
                        release(&gpu_flush_lock);
                        return ret;
                }
        }

        /* 2) RESOURCE_FLUSH */
        {
                struct virtio_gpu_resource_flush *cmd  = &flush_cmd.fl;
                struct virtio_gpu_ctrl_hdr       *resp = &flush_resp;

                memset(cmd, 0, sizeof(*cmd));
                memset(resp, 0, sizeof(*resp));

                cmd->hdr.type     = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
                cmd->r.x          = x;
                cmd->r.y          = y;
                cmd->r.width      = w;
                cmd->r.height     = h;
                cmd->resource_id  = gpu->resource_id;

                ret = virtqueue_send(gpu->controlq,
                                     cmd,  sizeof(*cmd),
                                     resp, sizeof(*resp));

                if (ret == 0 && resp->type != VIRTIO_GPU_RESP_OK_NODATA)
                        ret = -1;
        }

        release(&gpu_flush_lock);
        return ret;
}

void gpu_update(struct virtio_gpu_device *gpu, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    virtio_gpu_flush(gpu, x, y, w, h);
}

