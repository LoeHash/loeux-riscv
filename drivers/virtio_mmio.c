#include <virtio_mmio.h>
#include <slab.h>
#include <panic.h>
#include <spinlock.h>
#include <lib.h>
#include <memory.h>
#include <riscv.h>
#include <printk.h>

static int vq_alloc_desc_chain(struct virtqueue_n* vq);
static int vq_fill_desc_chain(struct virtqueue_n *vq, uint32_t start_idx);
static int vq_alloc_avail_ring(struct virtqueue_n* vq);
static int vq_alloc_used_ring(struct virtqueue_n* vq);
static int vq_find_free_desc(struct virtqueue_n *vq);
int virtqueue_send(struct virtqueue_n *vq,
                   void *cmd,  uint32_t cmd_len,
                   void *resp, uint32_t resp_len);

int init_virtqueue(struct virtqueue_n *vq, uint32_t start_idx)
{
        
        int ret = 0;

        if (vq == NULL) {
                return VIRTIO_QUEUE_INIT_ERROR_VQ_IS_NULL;
        }

        // 分配 desc
        if ((ret = vq_alloc_desc_chain(vq))) {
                return ret;
        }
        
        // 填充desc
        if ((ret = vq_fill_desc_chain(vq, start_idx))) {
                return ret;
        }

        // 创建avail_ring
        if ((ret = vq_alloc_avail_ring(vq))) {
                return ret;
        }

        // 创建 used ring
        if ((ret = vq_alloc_used_ring(vq))) {
                return ret;
        }
        
        return ret;
}

static int vq_alloc_used_ring(struct virtqueue_n* vq)
{        
        if (vq == NULL) {
                return -1;
        }

        // 分配used ring
        int used_ring_size = vq->queue_size * sizeof(struct virtq_used_elem);
        struct virtq_used_n *vu = slab_alloc(sizeof(struct virtq_used_n) + used_ring_size); 
        vq->used_start = vu;
        
        memset((void *)vq->used_start,  0, sizeof(struct virtq_used_n)  + used_ring_size);
        vq->used_phy = (phys_addr_t) vu;
        return 0;
}

static int vq_alloc_avail_ring(struct virtqueue_n* vq)
{        
        if (vq == NULL) {
                return -1;
        }
        // 分配avail
        int avail_ring_size = vq->queue_size * sizeof(uint16_t);
        struct virtq_avail_n *va = slab_alloc(sizeof(struct virtq_avail) + avail_ring_size); 
        vq->avail_start = va;
        
        memset((void *)vq->avail_start, 0, sizeof(struct virtq_avail_n) + avail_ring_size);
        // 设置物理地址
        // 内核建立恒等映射
        vq->avail_phy = (phys_addr_t) va;
        return 0;
}


static int vq_alloc_desc_chain(struct virtqueue_n* vq){
        // too many.
        if (vq->queue_size > 64 || vq->queue_size <= 0) {
                return VIRTIO_QUEUE_INIT_ERROR_VQ_DESC_TOO_LONG;
        }

        // 初始化队列锁
        uint64_t desc_size = sizeof(struct virtq_desc_n) * vq->queue_size;
        
        if (desc_size > 2048) {
                vq->desc_start = alloc_page();
        }else{
                vq->desc_start = slab_alloc(desc_size);
        }
        vq->desc_phy = (phys_addr_t)vq->desc_start;
        return 0;
}

static int vq_fill_desc_chain(struct virtqueue_n *vq, uint32_t start_idx)
{
        for (uint32_t i = 0; i < vq->queue_size; i++) {
                vq->desc_start[i].next  = (i + 1) % vq->queue_size;
                vq->desc_start[i].flags = 0;
                vq->desc_start[i].addr  = 0;
                vq->desc_start[i].len   = 0;
        }
        return 0;
}


struct virtqueue_n *alloc_virtqueue(int queue_size)
{
        struct virtqueue_n *vq = slab_alloc(sizeof(struct virtqueue_n));

        if (vq == NULL) {
                panic_error("Failed to allocate virtqueue\n");
        }

        vq->queue_size = queue_size;

        vq->free_desc_bit_map = 0;
        
        return vq;
}


int virtqueue_send(struct virtqueue_n *vq,
                   void *cmd,  uint32_t cmd_len,
                   void *resp, uint32_t resp_len)
{
        if (vq == NULL || cmd == NULL || resp == NULL)
                return -1;

        // 1. 找两个空闲描述符 
        int i0 = vq_find_free_desc(vq);
        if (i0 < 0)
                return -1;

        int i1 = vq_find_free_desc(vq);
        if (i1 < 0) {
                vq->free_desc_bit_map &= ~(1ULL << i0);
                return -1;
        }

        // 2. 填 desc[i0]：命令，驱动→设备
        vq->desc_start[i0].addr  = (phys_addr_t)(cmd);
        vq->desc_start[i0].len   = cmd_len;
        vq->desc_start[i0].flags = VRING_DESC_F_NEXT;
        vq->desc_start[i0].next  = i1;

        // 3. 填 desc[i1]：响应，设备→驱动
        vq->desc_start[i1].addr  = (phys_addr_t)(resp);
        vq->desc_start[i1].len   = resp_len;
        vq->desc_start[i1].flags = VRING_DESC_F_WRITE;
        vq->desc_start[i1].next  = 0;

        // 4. 挂到 avail ring
        //
        // 必须在【通知设备之前】快照 used 索引！
        // QEMU 对 QUEUE_NOTIFY 的写是同步处理的：b32_write 返回前
        // 设备已经处理完命令并把 used->idx 加 1。若 notify 之后才
        // 读 old_used，会直接读到完成后的值，while 等待下一次递增，
        // 永远等不到（表现为 used_idx 发送前后都是 1，最后超时）。
        uint16_t old_used = vq->used_start->idx;

        uint16_t aidx = vq->avail_start->idx % vq->queue_size;
        vq->avail_start->ring[aidx] = (uint16_t)i0;
        MEMORY_FENCE;
        vq->avail_start->idx++;
        MEMORY_FENCE; // 保证设备看到最新的 avail->idx 和 ring 内容

        // 5. NOTIFY!
        b32_write(vq->mmio_base + VIRTIO_MMIO_QUEUE_NOTIFY_OFFSET,
                  vq->queue_idx);

        // 6. 轮询 used ring，等设备处理完
        int timeout = 10000000;

        while (vq->used_start->idx == old_used) {
                if (--timeout == 0) {
                        vq->free_desc_bit_map &= ~(1ULL << i0);
                        vq->free_desc_bit_map &= ~(1ULL << i1);
                        printk("virtqueue_send timeout: i0=%d i1=%d "
                               "avail_idx=%u used_idx=%u old_used=%u\n",
                                i0, i1, vq->avail_start->idx,
                                vq->used_start->idx, old_used);
                        return -1;
                }
                wfi();
        }

        MEMORY_FENCE;

        // 7. 释放描述符
        vq->free_desc_bit_map &= ~(1ULL << i0);
        vq->free_desc_bit_map &= ~(1ULL << i1);

        return 0;
}

static int vq_find_free_desc(struct virtqueue_n *vq)
{
        for (int i = 0; i < vq->queue_size; i++) {
                if (!(vq->free_desc_bit_map & (1ULL << i))) {
                        vq->free_desc_bit_map |= (1ULL << i);
                        return i;
                }
        }
        return -1;
}