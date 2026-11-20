/**
 * Arctian basit bellek ayirici (HEAP bolgesi)
 */
#include "alloc.h"
#include "memory_map.h"
#include <stdint.h>

typedef struct block {
    uint32_t magic;
    uint32_t free;
    uint64_t size;
    struct block *next;
    uint64_t pad;
} block_t;

#define BLK_MAGIC 0xA110C0DEu
#define BLK_HDR   ((size_t)sizeof(block_t))

static uintptr_t heap_ptr = HEAP_BASE;
static block_t  *free_list = 0;
static size_t    used_bytes = 0;

/* SMP (C4): free_list/heap_ptr/used_bytes paylasili durumdur. kmalloc
 * kesmeler kapaliyken de cagrilabildigi icin (ve bir kenar yoldan bile
 * olsa kullanan oldugu icin) kilit RFLAGS kaydedip cli ile doner; ayni
 * CPU'da kesme isleyicisinin ikinci kmalloc'undan kaynaklanan korluyusu
 * onler. Esik kritik bolge sadece liste taramasidir; uyku yoktur. */
static volatile int g_alloc_lock;

static void *kmalloc_inner(size_t size);
static void kfree_inner(void *ptr);

static inline uint64_t alloc_lock(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    while (__sync_lock_test_and_set(&g_alloc_lock, 1))
        __asm__ volatile("pause");
    return flags;
}

static inline void alloc_unlock(uint64_t flags) {
    __sync_lock_release(&g_alloc_lock);
    if (flags & 0x200ull) __asm__ volatile("sti" ::: "memory");
}

void *kmalloc(size_t size) {
    uint64_t al = alloc_lock();
    void *r = kmalloc_inner(size);
    alloc_unlock(al);
    return r;
}

static void *kmalloc_inner(size_t size) {
    if (size == 0) size = 1;
    /* Yuvarlama tasmayi gizliyordu: SIZE_MAX'a yakin istek (size+15)&~15 ile
     * kucuk sayiya donusup basarisizlik yerine kucuk blok dondururdu. Butun
     * yiginden buyuk istek zaten karsilanamaz; bastan red. */
    if (size > (size_t)HEAP_MAX) return 0;
    size = (size + 15) & ~(size_t)15;

    block_t **pp = &free_list;
    while (*pp) {
        block_t *b = *pp;
        if (b->size >= size) {
            size_t rem = b->size - size;
            if (rem >= BLK_HDR + 16) {
                block_t *nb = (block_t *)((char *)(b + 1) + size);
                nb->magic = BLK_MAGIC;
                nb->free = 1;
                nb->size = rem - BLK_HDR;
                nb->next = b->next;
                b->size = size;
                b->next = nb;
            }
            *pp = b->next;
            b->free = 0;
            b->next = 0;
            used_bytes += b->size;
            return (void *)(b + 1);
        }
        pp = &b->next;
    }

    uintptr_t p = (heap_ptr + 15) & ~(uintptr_t)15;
    if (p + BLK_HDR + size > HEAP_BASE + HEAP_MAX) return 0;
    block_t *b = (block_t *)p;
    b->magic = BLK_MAGIC;
    b->free = 0;
    b->size = size;
    b->next = 0;
    heap_ptr = p + BLK_HDR + size;
    used_bytes += size;
    return (void *)(b + 1);
}

void kfree(void *ptr) {
    if (!ptr) return;
    uint64_t al = alloc_lock();
    kfree_inner(ptr);
    alloc_unlock(al);
}

static void kfree_inner(void *ptr) {
    if (!ptr) return;
    block_t *b = (block_t *)((char *)ptr - BLK_HDR);
    if (b->magic != BLK_MAGIC || b->free) return;
    b->free = 1;
    if (used_bytes >= b->size) used_bytes -= b->size;

    block_t **pp = &free_list;
    block_t *prev = 0;
    while (*pp && (uintptr_t)*pp < (uintptr_t)b) { prev = *pp; pp = &(*pp)->next; }
    b->next = *pp;
    *pp = b;

    if (b->next && (char *)(b + 1) + b->size == (char *)b->next) {
        b->size += BLK_HDR + b->next->size;
        b->next = b->next->next;
    }
    if (prev && (char *)(prev + 1) + prev->size == (char *)b) {
        prev->size += BLK_HDR + b->size;
        prev->next = b->next;
    }
}

size_t ksize(void *ptr) {
    if (!ptr) return 0;
    block_t *b = (block_t *)((char *)ptr - BLK_HDR);
    if (b->magic != BLK_MAGIC) return 0;
    return (size_t)b->size;
}

size_t kmalloc_used(void) {
    return used_bytes;
}

size_t kmalloc_total(void) {
    return (size_t)HEAP_MAX;
}
