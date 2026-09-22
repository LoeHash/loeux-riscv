#include <slab.h>
#include <printk.h>
#include <panic.h>
#include <spinlock.h>
#include <memory.h>
#include <math.h>
/*
Cache
 │
 │ 管理很多
 ▼
Slab
 │
 │ 包含很多
 ▼
Object
*/

static slab_global_cache_t slab_global_cache = {0};
static slab_meta_pool_t slab_meta_pool = {0};

static void init_slab_meta_pool();
static void slab_free_meta(slab_t* slab);
static slab_t* slab_alloc_meta();
static void inflate_slab_meta_pool(uint32_t page_size);
static void do_free_slab_object_from_out(slab_t* s, void* ptr);
static slab_t* do_create_new_slab(slab_t* father_slab);
static int do_create_slab_object_storage(slab_t* s);
static inline uint32_t slab_round_up(uint32_t size);
unsigned int __ctzdi2(unsigned long long x);

extern char _phy_start[];

/**
 * @brief 初始化 slab meta pool
 *
 * slab_meta_pool 专门用于分配 struct slab 元数据。
 *
 * 一个 page 被切分成：
 *
 *     slab_t -> slab_t -> slab_t -> ... -> NULL
 *
 * 后续 slab 元数据不足时，直接通过 alloc_page() 扩容，
 * 从而避免 slab 自己分配自己的元数据所产生的递归问题。
 */
static void init_slab_meta_pool()
{
	init_spinlock(&slab_meta_pool.lock);

	char* mem_page = alloc_page();
	if (mem_page == NULL) {
		panic_error("slab meta pool init failed\n");
	}

	uint64_t slab_count = PG_4K_SIZE / sizeof(slab_t);

	slab_meta_pool.freelist = mem_page;
	slab_meta_pool.total = slab_count;
	slab_meta_pool.free = slab_count;

	// 初始化当前 page 中的 slab_t freelist
	for (uint64_t i = 0; i < slab_count - 1; i++) {
		char* current = mem_page + i * sizeof(slab_t);
		char* next = mem_page + (i + 1) * sizeof(slab_t);

		*(uint64_t*)current = (uint64_t)next;
	}

	// 最后一个 slab_t
	char* last = mem_page + (slab_count - 1) * sizeof(slab_t);

	*(uint64_t*)last = 0;

	slab_meta_pool.end = (slab_t*)last;

	/*
	 * 初始化多个 page。
	 *
	 * 注意：
	 * 当前第一个 page 已经在上面初始化，
	 * 所以这里只需要再扩容 INIT_PAGE_SIZE - 1 个 page。
	 */
	if (SLAB_META_POOL_INIT_PAGE_SIZE > 1) {
		inflate_slab_meta_pool(SLAB_META_POOL_INIT_PAGE_SIZE - 1);
	}
}

/// @brief 释放 slab 元数据
/// @param slab 要释放的 slab 元数据指针
static void slab_free_meta(slab_t* slab)
{

	// 挂到 freelist 头部
	acquire(&slab_meta_pool.lock);

	// *(uint64_t *)slab = slab_meta_pool.freelist;
	*(void**)slab = slab_meta_pool.freelist;
	slab_meta_pool.freelist = (char*)slab;
	slab_meta_pool.free++;

	release(&slab_meta_pool.lock);
}

/// @brief free == 1 时提前扩容，保证正常分配永远不会把 pool 消耗到 0
/// @return slab_t * 分配到的 slab 元数据指针
static slab_t* slab_alloc_meta()
{
	// 检查是否需要扩容
	if (slab_meta_pool.free == 1) {
		acquire(&slab_meta_pool.lock);
		if (slab_meta_pool.free == 1) {
			// 扩容 2*64 个 slab
			inflate_slab_meta_pool(2);
		}
		release(&slab_meta_pool.lock);
	}

	acquire(&slab_meta_pool.lock);
	slab_t* slab = (slab_t*)slab_meta_pool.freelist;
	// slab_meta_pool.freelist = *(uint64_t *)slab;
	slab_meta_pool.freelist = *(void**)slab;
	slab_meta_pool.free--;
	release(&slab_meta_pool.lock);
	return slab;
}

/**
 * @brief 扩容 slab meta pool
 *
 * 每个 page 被切分成多个 slab_t，
 * 然后追加到当前 freelist 尾部。
 * @note 调用者必须持有锁 !LOCKED
 * @param page_size 要增加的 page 数量
 */
static void inflate_slab_meta_pool(uint32_t page_size)
{
	if (page_size == 0) {
		return;
	}

	uint32_t expand_page_size = page_size;

	char* current = (char*)slab_meta_pool.end;
	char* last = NULL;

	while (page_size > 0) {
		char* mem_page = alloc_page();

		if (mem_page == NULL) {
			panic_error("memory out of space!\n");
		}

		uint64_t slab_count = PG_4K_SIZE / sizeof(slab_t);

		/*
		 * 如果原来的 pool 非空，
		 * 把旧 freelist 尾节点连接到新 page。
		 */
		if (current != NULL) {
			*(uint64_t*)current = (uint64_t)mem_page;
		} else {
			/*
			 * 原 pool 为空，
			 * 新 page 成为 freelist 的头。
			 */
			slab_meta_pool.freelist = mem_page;
		}

		/*
		 * 初始化新 page 内部的 slab_t 链。
		 */
		for (uint64_t i = 0; i < slab_count - 1; i++) {
			char* slab = mem_page + i * sizeof(slab_t);

			char* next = mem_page + (i + 1) * sizeof(slab_t);

			*(uint64_t*)slab = (uint64_t)next;
		}

		/*
		 * 当前 page 的最后一个 slab_t。
		 */
		last = mem_page + (slab_count - 1) * sizeof(slab_t);

		current = last;

		page_size--;
	}

	*(uint64_t*)last = 0;

	slab_meta_pool.end = (slab_t*)last;

	uint64_t added =
	    (uint64_t)expand_page_size * (PG_4K_SIZE / sizeof(slab_t));

	slab_meta_pool.total += added;
	slab_meta_pool.free += added;
}

/// @brief 初始化 slab模块, 必须在 memory 初始化完成后/
void init_slab()
{
	init_spinlock(&slab_global_cache.lock);
	// 初始化 slab meta pool
	init_slab_meta_pool();

	// 8 ~ 4096
	for (int i = 0; i < 10; i++) {
		// 分配一个 slab 元数据
		slab_global_cache.slabs[i] = slab_alloc_meta();
		slab_t* s = slab_global_cache.slabs[i];

		s->object_size = 8 << i;
		s->next = NULL;
		s->total = PG_4K_SIZE / s->object_size;
		s->free = s->total;
		init_spinlock(&s->lock);

		// 构建 object storage
		char* mem_page = alloc_page();
		if (mem_page == NULL) {
			panic_error("slab init object storage failed\n");
		}
		char* free_block = mem_page;

		struct page* pg = PHY_TO_PAGE(mem_page);
		pg->slab = s;

		// 针对这个 slab，初始化 object storage
		s->freelist = free_block;
		for (int i = 0; i < s->total - 1; i++) {
			char* current = free_block + i * s->object_size;
			char* next = free_block + (i + 1) * s->object_size;

			*(uint64_t*)current = (uint64_t)next;
		}

		// 最后一个为NULL
		char* last = free_block + (s->total - 1) * s->object_size;
		*(uint64_t*)last = 0;
	}
}

/// @brief slab内存分配器专门分配小块内存地址
///        因此uint32足够，不会分配大于2048的内存
/// @param size
/// @return the address of mem block, returns null when error.
void* slab_alloc(uint32_t size)
{
	uint32_t round_size;
	if ((round_size = slab_round_up(size)) == 0) {
		return NULL;
	}
	// 获取round_size对应的父亲slab索引
	// 需要-3，因为round_size是8的倍数
	int father_slab_idx = POW2_SHIFT_COUNT(round_size) - 3;
	if (father_slab_idx < 0 || father_slab_idx > 9) {
		return NULL;
	}
	slab_t* s = slab_global_cache.slabs[father_slab_idx];

	while (1) {
		acquire(&s->lock);

		/*
		 * 当前 slab 有空闲对象，直接分配。
		 */
		if (!SLAB_IS_FULL(s)) {
			char* mem_block = s->freelist;

			s->freelist = *(char**)mem_block;
			s->free--;

			release(&s->lock);
			return mem_block;
		}

		/*
		 * 当前 slab 已满。
		 *
		 * next 的检查必须和创建/挂链放在同一个锁保护范围内，
		 * 否则多核下可能有两个 CPU 同时创建新 slab。
		 */
		if (s->next != NULL) {
			slab_t* next = s->next;

			release(&s->lock);

			s = next;
			continue;
		}

		/*
		 * 当前 slab 是最后一个 slab，而且已经满了。
		 *
		 * 此时仍然持有 s->lock。
		 * 其他 CPU 无法同时进入这里，因此只有一个 CPU
		 * 会创建并挂载新的 slab。
		 */
		slab_t* new_slab = do_create_new_slab(s);

		if (new_slab == NULL) {
			release(&s->lock);
			return NULL;
		}

		s->next = new_slab;

		release(&s->lock);

		s = new_slab;
	}
}

/// @brief 释放 slab 内存
/// @param ptr 要释放的内存地址
/// @note slab_free() 只接受 slab_alloc() 返回的地址
void slab_free(void* ptr)
{
	if (ptr == NULL) {
		panic_error("slab_free: invalid ptr\n");
	}

	struct page* pg = PHY_TO_PAGE(ptr);
	if (pg == NULL) {
		panic_error("slab_free: invalid ptr\n");
	}

	slab_t* s = pg->slab;
	if (s == NULL) {
		// 说明不是走的slab
		// 可以走free_page
		panic_error("slab_free: ptr is not a slab object\n");
		return;
	}

	uintptr_t offset = (uintptr_t)ptr - pg->paddr;
	if (offset >= PG_4K_SIZE || offset % s->object_size != 0) {
		panic_error("slab_free: invalid slab object\n");
	}
	do_free_slab_object_from_out(s, ptr);
}

/// @brief 将ptr挂回到s的freelist头部
/// @param s 目标 slab 元数据指针
/// @param ptr 要释放的内存地址
static void do_free_slab_object_from_out(slab_t* s, void* ptr)
{
	acquire(&s->lock);

	*(void**)ptr = s->freelist;
	s->freelist = ptr;
	s->free++;

	release(&s->lock);
}

/// @brief 创建一个新的 slab
/// @param father_slab 父亲slab
/// @return 新的 slab
static slab_t* do_create_new_slab(slab_t* father_slab)
{
	slab_t* s = slab_alloc_meta();

	if (s == NULL)
		return NULL;

	s->object_size = father_slab->object_size;
	s->total = father_slab->total;
	s->free = s->total;
	s->next = NULL;

	init_spinlock(&s->lock);

	if (do_create_slab_object_storage(s) != 0) {
		printk("do_create_slab_object_storage failed\n");
		slab_free_meta(s);
		return NULL;
	}

	return s;
}

/// @brief 创建 slab 的 object storage
/// @param s 新的 slab
static int do_create_slab_object_storage(slab_t* s)
{
	// 分配一个 slab 元数据
	char* mem_page = alloc_page();
	if (mem_page == NULL) {
		return -1;
	}

	char* free_block = mem_page;

	struct page* pg = PHY_TO_PAGE(mem_page);
	pg->slab = s;

	// 针对这个 slab，初始化 object storage
	s->freelist = free_block;
	for (int i = 0; i < s->total - 1; i++) {
		char* current = free_block + i * s->object_size;
		char* next = free_block + (i + 1) * s->object_size;

		*(uint64_t*)current = (uint64_t)next;
	}

	// 最后一个为NULL
	char* last = free_block + (s->total - 1) * s->object_size;
	*(uint64_t*)last = 0;
	return 0;
}

/// @brief slab 对齐大小
/// @param size 大小
/// @return 对齐后的大小
static inline uint32_t slab_round_up(uint32_t size)
{
	if (size == 0 || size > SLAB_SIZE(4096))
		return 0;

	if (size <= SLAB_SIZE(8))
		return SLAB_SIZE(8);

	size--;
	size |= size >> 1;
	size |= size >> 2;
	size |= size >> 4;
	size |= size >> 8;
	size |= size >> 16;
	size++;

	return size;
}

/// @brief 计算 x 中最低位 0 的个数
/// @param x 输入值
/// @return 输出值
/// @note 为了编译通过.... stupid compiler.. and stupid trick
unsigned int __ctzdi2(unsigned long long x)
{
	unsigned int ret = 0;
	if (!x)
		return 64;
	if (!(x & 0xffffffff)) {
		x >>= 32;
		ret |= 32;
	}
	if (!(x & 0xffff)) {
		x >>= 16;
		ret |= 16;
	}
	if (!(x & 0xff)) {
		x >>= 8;
		ret |= 8;
	}
	if (!(x & 0xf)) {
		x >>= 4;
		ret |= 4;
	}
	if (!(x & 0x3)) {
		x >>= 2;
		ret |= 2;
	}
	if (!(x & 0x1)) {
		x >>= 1;
		ret |= 1;
	}
	return ret;
}

void slab_dump(void)
{
	printk("\n");
	printk(
	    "============================================================\n");
	printk("                     SLAB DUMP\n");
	printk(
	    "============================================================\n");

	for (int i = 0; i < 10; i++) {
		slab_t* s = slab_global_cache.slabs[i];

		printk("\n");
		printk("[SIZE CLASS] %lu bytes\n", s->object_size);

		int slab_count = 0;

		while (s != NULL) {
			acquire(&s->lock);

			uint64_t used = s->total - s->free;
			slab_t* next = s->next;

			printk("  slab[%d]\n", slab_count);
			printk("    addr      : %p\n", s);
			printk("    object    : %lu\n", s->object_size);
			printk("    total     : %lu\n", s->total);
			printk("    free      : %lu\n", s->free);
			printk("    used      : %lu\n", used);
			printk("    freelist  : %p\n", s->freelist);
			printk("    next      : %p\n", next);

			release(&s->lock);

			s = next;
			slab_count++;
		}

		printk("  slab count : %d\n", slab_count);
	}

	printk("\n");
	printk("[SLAB META POOL]\n");

	acquire(&slab_meta_pool.lock);

	printk("  total       : %lu\n", slab_meta_pool.total);
	printk("  free        : %lu\n", slab_meta_pool.free);
	printk("  used        : %lu\n",
	       slab_meta_pool.total - slab_meta_pool.free);
	printk("  freelist    : %p\n", slab_meta_pool.freelist);
	printk("  end         : %p\n", slab_meta_pool.end);

	release(&slab_meta_pool.lock);

	printk("\n");
	printk(
	    "============================================================\n");
}