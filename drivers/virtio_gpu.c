#include <virtio_gpu.h>
#include <virtio_mmio.h>
#include <memlayout.h>
#include <slab.h>
#include <lib.h>
#include <panic.h>
#include <printk.h>

static struct virtio_gpu_device root_gpu_device = {0};
static struct virtio_gpu_device *gpu_get_last();

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

void init_virtio_gpu(){
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


        // 
        printk("before get display info, the gpu:\n");
        dump_gpu(gpu);
        printk("magic=%0#x version=%u devid=%u\n",
                b32_read(base + VIRTIO_MMIO_MAGIC_VALUE_OFFSET),
                b32_read(base + VIRTIO_MMIO_VERSION_OFFSET),
                b32_read(base + VIRTIO_MMIO_DEVICE_ID_OFFSET));


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

        
        printk("QUEUE_NUM macro=%d\n", VIRTIO_GPU_MAX_QUEUE_NUM);
        printk("desc_phy=%0#lx avail_phy=%0#lx used_phy=%0#lx\n",
        gpu->controlq->desc_phy, gpu->controlq->avail_phy, gpu->controlq->used_phy);
        printk("cmd=%0#lx resp=%0#lx\n", (uint64_t)cmd, (uint64_t)resp);

        uintptr_t base = gpu->mmio_base;
        b32_write(base + VIRTIO_MMIO_QUEUE_SEL_OFFSET, 0);   // 选中 controlq

        /*
         * 注意：QUEUE_NUM / QUEUE_DESC_* / QUEUE_AVAIL_* / QUEUE_USED_*
         * 在 virtio-mmio 规范里是【只写】寄存器，QEMU 读它们恒返回 0，
         * 不能用读回值判断配置是否生效。可读的队列状态只有
         * QUEUE_NUM_MAX（设备能力）和 QUEUE_READY（是否已激活）。
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
        dump_gpu(gpu);
        while(1);
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