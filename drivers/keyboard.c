#include <keyboard.h>
#include <printk.h>
#include <lib.h>
#include <virtio_mmio.h>
#include <slab.h>
#include <panic.h>
#include <riscv.h>

static struct virtio_input_device root_keyboard_device = {0};
static int keyboard_device_count = 0;

static struct virtio_input_device *keyboard_get_last(void);
static int  virtio_keyboard_init(struct virtio_input_device *kb);
static void virtio_keyboard_handshake(uintptr_t base);
static int  virtio_keyboard_negotiate_features(uintptr_t base);
static int  virtio_keyboard_setup_eventq(struct virtio_input_device *kb);
static void virtio_keyboard_driver_ok(uintptr_t base);
static int  virtio_keyboard_submit_event(struct virtio_input_device *kb, int idx);
static void detect_keyboard_device(void);
static void init_all_keyboard(void);

static struct virtio_input_event ev_bufs[INPUT_EVENT_BUFS];


void init_keyboard(void)
{
	printk("init_keyboard\n");
	detect_keyboard_device();
	init_all_keyboard();
}

static void init_all_keyboard(void)
{
	struct virtio_input_device *now = root_keyboard_device.next;

	while (now != NULL) {
		int ret = virtio_keyboard_init(now);
		if (ret != 0 || now->initialized != 1)
			panic_error("init_all_keyboard: keyboard init failed!\n");

		now->initialized = 1;
		now = now->next;
	}
}

static int virtio_keyboard_init(struct virtio_input_device *kb)
{
	uintptr_t base = kb->mmio_base;
	int ret;

	virtio_keyboard_handshake(base);

	if ((ret = virtio_keyboard_negotiate_features(base))) {
		printk("[Wrong] keyboard: negotiate features failed\n");
		return ret;
	}

	if ((ret = virtio_keyboard_setup_eventq(kb))) {
		printk("[Wrong] keyboard: setup eventq failed\n");
		return ret;
	}

	virtio_keyboard_driver_ok(base);

	for (int i = 0; i < INPUT_EVENT_BUFS; i++) {
		if (virtio_keyboard_submit_event(kb, i) < 0)
			return -1;
	}

	/* TEMP DEBUG: dump vring */
	{
		struct virtqueue_n *v = kb->eventq;
		printk("KBVQ qsize=%d desc_pa=%lx avail_pa=%lx used_pa=%lx\n",
		       v->queue_size, v->desc_phy, v->avail_phy, v->used_phy);
		printk("KBVQ avail flags=%u idx=%u ring=[%u %u %u %u %u %u %u %u]\n",
		       v->avail_start->flags, v->avail_start->idx,
		       v->avail_start->ring[0], v->avail_start->ring[1],
		       v->avail_start->ring[2], v->avail_start->ring[3],
		       v->avail_start->ring[4], v->avail_start->ring[5],
		       v->avail_start->ring[6], v->avail_start->ring[7]);
		printk("KBVQ avail ring[8..11]=[%u %u %u %u] used_idx=%u evbuf_pa=%lx\n",
		       v->avail_start->ring[8], v->avail_start->ring[9],
		       v->avail_start->ring[10], v->avail_start->ring[11],
		       v->used_start->idx, (uint64_t)&ev_bufs[0]);
	}

	return 0;
}      kb->initialized = 1;
	return 0;
}

static void virtio_keyboard_handshake(uintptr_t base)
{
	b32_write(base + VIRTIO_MMIO_STATUS_OFFSET, 0);
	b32_write(base + VIRTIO_MMIO_STATUS_OFFSET, VIRTIO_STATUS_ACKNOWLEDGE);

	uint32_t status = b32_read(base + VIRTIO_MMIO_STATUS_OFFSET);
	b32_write(base + VIRTIO_MMIO_STATUS_OFFSET, status | VIRTIO_STATUS_DRIVER);
}

static int virtio_keyboard_negotiate_features(uintptr_t base)
{
	uint32_t feat_lo, feat_hi;

	b32_write(base + VIRTIO_MMIO_DEVICE_FEATURES_SEL_OFFSET, 0);
	feat_lo = b32_read(base + VIRTIO_MMIO_DEVICE_FEATURES_OFFSET);

	b32_write(base + VIRTIO_MMIO_DEVICE_FEATURES_SEL_OFFSET, 1);
	feat_hi = b32_read(base + VIRTIO_MMIO_DEVICE_FEATURES_OFFSET);

	b32_write(base + VIRTIO_MMIO_DRIVER_FEATURES_SEL_OFFSET, 0);
	b32_write(base + VIRTIO_MMIO_DRIVER_FEATURES_OFFSET, 0);

	b32_write(base + VIRTIO_MMIO_DRIVER_FEATURES_SEL_OFFSET, 1);
	b32_write(base + VIRTIO_MMIO_DRIVER_FEATURES_OFFSET, 1);

	uint32_t status = b32_read(base + VIRTIO_MMIO_STATUS_OFFSET);
	b32_write(base + VIRTIO_MMIO_STATUS_OFFSET, status | VIRTIO_STATUS_FEATURES_OK);

	if (!(b32_read(base + VIRTIO_MMIO_STATUS_OFFSET) & VIRTIO_STATUS_FEATURES_OK))
		return -1;

	return 0;
}

static int virtio_keyboard_setup_eventq(struct virtio_input_device *kb)
{
	uintptr_t base = kb->mmio_base;
	int ret;

	b32_write(base + VIRTIO_MMIO_QUEUE_SEL_OFFSET, 0);
	b32_write(base + VIRTIO_MMIO_QUEUE_NUM_OFFSET, VIRTIO_KEYBOARD_MAX_QUEUE_NUM);

	struct virtqueue_n *vq = alloc_virtqueue(VIRTIO_KEYBOARD_MAX_QUEUE_NUM);
	if ((ret = init_virtqueue(vq, 0)))
		panic_error("virtio_keyboard_setup_eventq: init_virtqueue failed\n");

	vq->mmio_base  = base;
	vq->queue_idx  = 0;
	vq->queue_size = VIRTIO_KEYBOARD_MAX_QUEUE_NUM;
	kb->eventq     = vq;

	uint64_t desc_phy  = vq->desc_phy;
	uint64_t avail_phy = vq->avail_phy;
	uint64_t used_phy  = vq->used_phy;

	b32_write(base + VIRTIO_MMIO_QUEUE_DESC_LOW_OFFSET,  GET_LOW_32(desc_phy));
	b32_write(base + VIRTIO_MMIO_QUEUE_DESC_HIGH_OFFSET, GET_HIGH_32(desc_phy));

	b32_write(base + VIRTIO_MMIO_QUEUE_AVAIL_LOW_OFFSET,  GET_LOW_32(avail_phy));
	b32_write(base + VIRTIO_MMIO_QUEUE_AVAIL_HIGH_OFFSET, GET_HIGH_32(avail_phy));

	b32_write(base + VIRTIO_MMIO_QUEUE_USED_LOW_OFFSET,  GET_LOW_32(used_phy));
	b32_write(base + VIRTIO_MMIO_QUEUE_USED_HIGH_OFFSET, GET_HIGH_32(used_phy));

	b32_write(base + VIRTIO_MMIO_QUEUE_READY_OFFSET, 1);

	return 0;
}

static void virtio_keyboard_driver_ok(uintptr_t base)
{
	uint32_t status = b32_read(base + VIRTIO_MMIO_STATUS_OFFSET);
	b32_write(base + VIRTIO_MMIO_STATUS_OFFSET, status | VIRTIO_STATUS_DRIVER_OK);
}

/*
 * 提交一个空事件缓冲区。desc 下标强制等于 buf 下标，
 * 这样 poll 里 used.id 直接就是 buf 下标。
 * 返回 desc 下标，失败 -1。
 */
static int virtio_keyboard_submit_event(struct virtio_input_device *kb, int idx)
{
	struct virtqueue_n *vq = kb->eventq;
	int d = idx;

	if (vq->free_desc_bit_map & (1ULL << d))
		return -1;

	vq->free_desc_bit_map |= (1ULL << d);

	vq->desc_start[d].addr  = (phys_addr_t)&ev_bufs[idx];
	vq->desc_start[d].len   = sizeof(struct virtio_input_event);
	vq->desc_start[d].flags = VRING_DESC_F_WRITE;
	vq->desc_start[d].next  = 0;

	uint16_t aidx = vq->avail_start->idx % vq->queue_size;
	vq->avail_start->ring[aidx] = (uint16_t)d;
	MEMORY_FENCE;
	vq->avail_start->idx++;
	MEMORY_FENCE;

	b32_write(vq->mmio_base + VIRTIO_MMIO_QUEUE_NOTIFY_OFFSET, vq->queue_idx);

	return d;
}

static void detect_keyboard_device(void)
{
	for (int i = 0; i < VIRTIO_MMIO_COUNT; i++) {
		if (*R_LEVEL(VIRTIO_MMIO_MAGIC_VALUE_OFFSET, i) != VIRTIO_MMIO_MAGIC)
			continue;

		if (*R_LEVEL(VIRTIO_MMIO_DEVICE_ID_OFFSET, i) != VIRTIO_KEYBOARD_DEVICE_ID)
			continue;

		if (*R_LEVEL(VIRTIO_MMIO_VERSION_OFFSET, i) != 2)
			continue;

                printk("found keyboard device at %p\n", GET_VIR_BASE(i));
		struct virtio_input_device *new_keyboard =
			slab_alloc(sizeof(struct virtio_input_device));

		if (new_keyboard == NULL)
			panic_error("detect_keyboard_device: alloc failed\n");

		memset(new_keyboard, 0, sizeof(struct virtio_input_device));
		new_keyboard->next = NULL;
		new_keyboard->mmio_base = GET_VIR_BASE(i);
		new_keyboard->initialized = 0;

		keyboard_get_last()->next = new_keyboard;
		keyboard_device_count++;
	}
}

static struct virtio_input_device *keyboard_get_last(void)
{
	struct virtio_input_device *tmp = &root_keyboard_device;
	struct virtio_input_device *next = root_keyboard_device.next;

	while (next != NULL) {
		tmp = next;
		next = tmp->next;
	}
	return tmp;
}


/* ---------------- 输入处理 ---------------- */

static const char keymap_lo[128] = {
	[1]  = 0x1B,
	[2]  = '1', [3]  = '2', [4]  = '3', [5]  = '4', [6]  = '5',
	[7]  = '6', [8]  = '7', [9]  = '8', [10] = '9', [11] = '0',
	[12] = '-', [13] = '=',
	[14] = '\b',
	[15] = '\t',
	[16] = 'q', [17] = 'w', [18] = 'e', [19] = 'r', [20] = 't',
	[21] = 'y', [22] = 'u', [23] = 'i', [24] = 'o', [25] = 'p',
	[26] = '[', [27] = ']',
	[28] = '\n',
	[30] = 'a', [31] = 's', [32] = 'd', [33] = 'f', [34] = 'g',
	[35] = 'h', [36] = 'j', [37] = 'k', [38] = 'l',
	[39] = ';', [40] = '\'',
	[41] = '`',
	[43] = '\\',
	[44] = 'z', [45] = 'x', [46] = 'c', [47] = 'v', [48] = 'b',
	[49] = 'n', [50] = 'm',
	[51] = ',', [52] = '.', [53] = '/',
	[57] = ' ',
};

static const char keymap_hi[128] = {
	[2]  = '!', [3]  = '@', [4]  = '#', [5]  = '$', [6]  = '%',
	[7]  = '^', [8]  = '&', [9]  = '*', [10] = '(', [11] = ')',
	[12] = '_', [13] = '+',
	[16] = 'Q', [17] = 'W', [18] = 'E', [19] = 'R', [20] = 'T',
	[21] = 'Y', [22] = 'U', [23] = 'I', [24] = 'O', [25] = 'P',
	[26] = '{', [27] = '}',
	[30] = 'A', [31] = 'S', [32] = 'D', [33] = 'F', [34] = 'G',
	[35] = 'H', [36] = 'J', [37] = 'K', [38] = 'L',
	[39] = ':', [40] = '"',
	[41] = '~',
	[43] = '|',
	[44] = 'Z', [45] = 'X', [46] = 'C', [47] = 'V', [48] = 'B',
	[49] = 'N', [50] = 'M',
	[51] = '<', [52] = '>', [53] = '?',
};

static int buf_full(struct virtio_input_device *kb)
{
	return (kb->tail + 1) % INPUT_CHAR_BUF_SIZE == kb->head;
}

static int buf_empty(struct virtio_input_device *kb)
{
	return kb->head == kb->tail;
}

static void buf_push(struct virtio_input_device *kb, char c)
{
	if (buf_full(kb))
		return;
	kb->buf[kb->tail] = c;
	kb->tail = (kb->tail + 1) % INPUT_CHAR_BUF_SIZE;
}

static int buf_pop(struct virtio_input_device *kb, char *out)
{
	if (buf_empty(kb))
		return -1;
	*out = kb->buf[kb->head];
	kb->head = (kb->head + 1) % INPUT_CHAR_BUF_SIZE;
	return 0;
}

static void handle_key(struct virtio_input_device *kb, uint16_t code, uint32_t value)
{
	if (code == KEY_LEFTSHIFT) {
		kb->shift = (value == 1);
		return;
	}

	if (value != 1)
		return;

	if (code >= 128)
		return;

	char c = kb->shift ? keymap_hi[code] : keymap_lo[code];
	if (c)
		buf_push(kb, c);
}

static int keyboard_poll(struct virtio_input_device *kb)
{
	struct virtqueue_n *vq = kb->eventq;
	int n = 0;

	while (vq->used_start->idx != kb->last_used_idx) {
		struct virtq_used_elem *e =
			&vq->used_start->ring[kb->last_used_idx % vq->queue_size];

		int buf_idx = e->id;

		struct virtio_input_event *ev = &ev_bufs[buf_idx];
		if (ev->type == VIRTIO_INPUT_EV_KEY)
			handle_key(kb, ev->code, ev->value);

		vq->free_desc_bit_map &= ~(1ULL << buf_idx);
		kb->last_used_idx++;

		virtio_keyboard_submit_event(kb, buf_idx);

		n++;
	}

	return n;
}

int keyboard_getchar(char *out)
{
	struct virtio_input_device *kb = root_keyboard_device.next;
	if (kb == NULL){
                printk("device is null!\n");
                return -1;
        }
	while (buf_empty(kb)) {
		keyboard_poll(kb);
		if (!buf_empty(kb))
			break;
		wfi();
	}

	return buf_pop(kb, out);
}