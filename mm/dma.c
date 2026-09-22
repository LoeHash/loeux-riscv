#include <memlayout.h>
#include <memory.h>
#include <type.h>
#include <spinlock.h>
#include <dma.h>

static char* dma_buf_start = NULL;
static char* dma_buf_end = NULL;
static spinlock_t dma_alloc_lock = {0};

void init_dma()
{
	init_spinlock(&dma_alloc_lock);
	dma_buf_start = (char*)DMA_START_BASE;
	dma_buf_end = (char*)DMA_END;
}

void* dma_alloc(uint64_t size)
{
	acquire(&dma_alloc_lock);
	if (dma_buf_start + size > dma_buf_end) {
		release(&dma_alloc_lock);
		return NULL;
	}
	char* buf = dma_buf_start;
	dma_buf_start += size;
	release(&dma_alloc_lock);
	return buf;
}
