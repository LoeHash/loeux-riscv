#include <virtio_mmio.h>
#include <slab.h>
#include <panic.h>
#include <spinlock.h>
static int vq_alloc_desc_chain(struct virtqueue_n* vq);
static int vq_fill_desc_chain(struct virtqueue_n *vq, uint32_t start_idx);
static int vq_alloc_avail_ring(struct virtqueue_n* vq);

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
        
        
        return ret;
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

        return 0;
}

static int vq_fill_desc_chain(struct virtqueue_n *vq, uint32_t start_idx){
        struct virtq_desc_n *start = vq->desc_start, *end = vq->desc_start + vq->queue_size;

        for (int i = start_idx; start < end; i++, start++) {
                start->next = i;
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
        
        return vq;
}
