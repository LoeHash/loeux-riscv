#include <virtio_gpu.h>
#include <virtio_mmio.h>
#include <memlayout.h>
#include <slab.h>
#include <lib.h>
#include <panic.h>

static struct virtio_gpu_device root_gpu_device = {0};
static struct virtio_gpu_device *gpu_get_last();
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

static void init_all_gpu(){

}

static int virtio_gpu_init(struct virtio_gpu_device *gpu)
{
        uintptr_t base = gpu->mmio_base;
        int ret;

            virtio_gpu_handshake(base);

            if ((ret = virtio_gpu_negotiate_features(base)))
                return ret;

            if ((ret = virtio_gpu_setup_controlq(gpu)))
                return ret;

            if ((ret = virtio_gpu_setup_cursorq(gpu)))
                return ret;

            virtio_gpu_driver_ok(base);

            if ((ret = virtio_gpu_get_display_info(gpu)))
                return ret;

            if ((ret = virtio_gpu_create_framebuffer(gpu)))
                return ret;

        gpu->initialized = 1;
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

}

static int virtio_gpu_negotiate_features(uintptr_t base){

}

static int virtio_gpu_setup_controlq(struct virtio_gpu_device *gpu){
        
}

static int virtio_gpu_setup_cursorq(struct virtio_gpu_device *gpu){

}

static void virtio_gpu_driver_ok(uintptr_t base){

}

static int virtio_gpu_get_display_info(struct virtio_gpu_device *gpu){

}

static int virtio_gpu_create_framebuffer(struct virtio_gpu_device *gpu){

}