/**
 * Arctian dinamik bellek ayirici (HEAP + Higher-Half VMM Genislemeli Havuz)
 */
#include "alloc.h"
#include "memory_map.h"
#include "paging.h"
#include <stdint.h>

extern uint64_t vmm_alloc_frame(void) __attribute__((weak));
extern void     vmm_free_frame(uint64_t phys) __attribute__((weak));
extern int      vmm_map_kernel_page(uint64_t va, uint64_t pa, uint64_t flags) __attribute__((weak));
extern uint64_t vmm_translate(vmm_space_t sp, uint64_t va) __attribute__((weak));
extern int      vmm_unmap(vmm_space_t sp, uint64_t va) __attribute__((weak));
extern uint64_t vmm_avail_above_reserve_bytes(void) __attribute__((weak));

#define ALLOC_DYN_CHUNK_MIN  (256ull * 1024ull)
#define ALLOC_MAX_SINGLE     (1024ull * 1024ull * 1024ull)

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
static uintptr_t dyn_ptr  = VMM_KHEAP_BASE;
static size_t    dyn_mapped = 0;
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

static void insert_free_block(block_t *b) {
    b->free = 1;
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

static int kmalloc_expand_vmm(size_t min_block_bytes) {
    if (!vmm_alloc_frame || !vmm_free_frame || !vmm_map_kernel_page) return 0;
    vmm_state_t *st = VMM_STATE;
    if (!st || st->magic != VMM_MAGIC || !st->ready) return 0;

    size_t exact = (BLK_HDR + min_block_bytes + 0xFFFull) & ~(size_t)0xFFFull;
    size_t chunk = (exact < ALLOC_DYN_CHUNK_MIN) ? (size_t)ALLOC_DYN_CHUNK_MIN : exact;

    for (int attempt = 0; attempt < 2; attempt++) {
        if (dyn_mapped + chunk > (size_t)VMM_KHEAP_MAX) {
            chunk = exact;
            if (dyn_mapped + chunk > (size_t)VMM_KHEAP_MAX) return 0;
        }
        size_t npages = chunk >> 12;
        size_t mapped = 0;
        int ok = 1;
        for (size_t i = 0; i < npages; i++) {
            uint64_t pa = vmm_alloc_frame();
            if (!pa) { ok = 0; break; }
            uint64_t va = (uint64_t)(dyn_ptr + (i << 12));
            if (vmm_map_kernel_page(va, pa, VMM_PTE_P | VMM_PTE_W) != 0) {
                vmm_free_frame(pa);
                ok = 0;
                break;
            }
            mapped++;
        }
        if (ok) {
            block_t *nb = (block_t *)dyn_ptr;
            nb->magic = BLK_MAGIC;
            nb->free  = 1;
            nb->size  = chunk - BLK_HDR;
            nb->next  = 0;
            nb->pad   = 0;
            insert_free_block(nb);
            dyn_ptr    += chunk;
            dyn_mapped += chunk;
            return 1;
        }
        /* Geri sar */
        for (size_t i = 0; i < mapped; i++) {
            uint64_t va = (uint64_t)(dyn_ptr + (i << 12));
            uint64_t pa = vmm_translate ? vmm_translate(st->kernel_space, va) : 0;
            if (vmm_unmap) vmm_unmap(st->kernel_space, va);
            if (pa) vmm_free_frame(pa);
        }
        if (chunk == exact) break;
        chunk = exact;
    }
    return 0;
}

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

static void *kmalloc_alloc_from_freelist(size_t size) {
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
    return 0;
}

static void *kmalloc_inner(size_t size) {
    if (size == 0) size = 1;
    if (size > (size_t)ALLOC_MAX_SINGLE) return 0;
    size = (size + 15) & ~(size_t)15;

    void *r = kmalloc_alloc_from_freelist(size);
    if (r) return r;

    uintptr_t p = (heap_ptr + 15) & ~(uintptr_t)15;
    if (p + BLK_HDR + size <= HEAP_BASE + HEAP_MAX) {
        block_t *b = (block_t *)p;
        b->magic = BLK_MAGIC;
        b->free = 0;
        b->size = size;
        b->next = 0;
        heap_ptr = p + BLK_HDR + size;
        used_bytes += size;
        return (void *)(b + 1);
    }

    /* Baslangic 63 MB bolgesi dolduysa VMM higher-half heap ile dinamik buyut */
    if (kmalloc_expand_vmm(size)) {
        return kmalloc_alloc_from_freelist(size);
    }
    return 0;
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
    if (used_bytes >= b->size) used_bytes -= b->size;
    insert_free_block(b);
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

size_t kmalloc_heap_mapped(void) {
    return (size_t)HEAP_MAX + dyn_mapped;
}

size_t kmalloc_total(void) {
    size_t base = (size_t)HEAP_MAX + dyn_mapped;
    if (vmm_avail_above_reserve_bytes) {
        uint64_t avail = vmm_avail_above_reserve_bytes();
        uint64_t max_dyn_rem = (VMM_KHEAP_MAX > dyn_mapped) ? (VMM_KHEAP_MAX - dyn_mapped) : 0;
        if (avail > max_dyn_rem) avail = max_dyn_rem;
        base += (size_t)avail;
    }
    return base;
}
