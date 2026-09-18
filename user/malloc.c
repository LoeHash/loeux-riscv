#include <malloc.h>
#include <utype.h>
#include <ulib.h>

static struct block *heap_head = NULL;
static struct block *heap_tail = NULL;
static umutex heap_lock = {0};

static void umutex_lock(umutex *m) {
    while (__sync_lock_test_and_set(&m->locked, 1)) {
        while (m->locked) ;
    }
    __sync_synchronize();
}

static void umutex_unlock(umutex *m) {
    __sync_synchronize();
    __sync_lock_release(&m->locked);
}

/* 向内核要内存，返回新块的起始地址（payload 尚未使用） */
static struct block *grow_heap(size_t need)
{
    size_t total = ALIGN_UP(need + BLK_HDR);

    void *p = sbrk((int64_t)total);
    if (p == (void *)-1)
        return NULL;

    /* 如果 sbrk 返回的是新 brk，改成: p = (char*)p - total; */
    struct block *b = (struct block *)p;
    b->size = total - BLK_HDR;
    b->free = 1;
    b->next = NULL;
    b->prev = heap_tail;

    if (heap_tail)
        heap_tail->next = b;
    else
        heap_head = b;
    heap_tail = b;
    return b;
}

void *malloc(size_t n)
{
    if (n == 0)
        return NULL;

    n = ALIGN_UP(n);
    umutex_lock(&heap_lock);

    /* first fit */
    struct block *b = heap_head;
    while (b) {
        if (b->free && b->size >= n) {
            b->free = 0;
            umutex_unlock(&heap_lock);
            return (char *)b + BLK_HDR;
        }
        b = b->next;
    }

    /* 没找到，扩堆 */
    b = grow_heap(n);
    if (!b) {
        umutex_unlock(&heap_lock);
        return NULL;
    }
    b->free = 0;
    umutex_unlock(&heap_lock);
    return (char *)b + BLK_HDR;
}

void free(void *ptr)
{
    if (!ptr)
        return;

    struct block *b = (struct block *)((char *)ptr - BLK_HDR);

    umutex_lock(&heap_lock);
    b->free = 1;

    /* 向后合并 */
    while (b->next && b->next->free) {
        b->size += BLK_HDR + b->next->size;
        b->next = b->next->next;
        if (b->next)
            b->next->prev = b;
    }

    /* 向前合并 */
    if (b->prev && b->prev->free) {
        b->prev->size += BLK_HDR + b->size;
        b->prev->next = b->next;
        if (b->next)
            b->next->prev = b->prev;
        else
            heap_tail = b->prev;
        b = b->prev;
    }

    if (b->next == NULL)
        heap_tail = b;

    umutex_unlock(&heap_lock);
}

void *calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (!p) return NULL;
    char *c = p;
    for (size_t i = 0; i < total; i++) c[i] = 0;
    return p;
}

void *realloc(void *ptr, size_t n)
{
    if (!ptr) return malloc(n);
    if (n == 0) { free(ptr); return NULL; }

    struct block *b = (struct block *)((char *)ptr - BLK_HDR);
    if (b->size >= n)
        return ptr;

    void *np = malloc(n);
    if (!np) return NULL;
    char *src = ptr, *dst = np;
    for (size_t i = 0; i < b->size; i++) dst[i] = src[i];
    free(ptr);
    return np;
}