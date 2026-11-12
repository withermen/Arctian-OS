#ifndef ARCTIAN_SYSTEM_SPINLOCK_H
#define ARCTIAN_SYSTEM_SPINLOCK_H

#include <stdint.h>
#include <stdbool.h>

/* Ticket spinlock + atomik temeller.
 *
 * Kilit sirasi (deadlock onleme, kucukten buyuge):
 *   percpu -> runqueue -> timer
 * Ayni sirada al; ters sirada alma.
 *
 * irq_save/irq_restore: kesme durumunu kaydeder. Kilit alirken
 * kesmeleri kapatmak icin:
 *     uint64_t f = irq_save();
 *     spin_lock(&l);
 *     ...
 *     spin_unlock(&l);
 *     irq_restore(f);
 *
 * Bu dosya yalnizca temel saglar; bu pakette hicbir yerde kullanilmaz. */

typedef struct {
    uint32_t next;      /* siradaki bilet */
    uint32_t owner;     /* iceri girmesine izin verilen bilet */
} spinlock_t;

#define SPINLOCK_INIT { 0u, 0u }

static inline void spin_lock(spinlock_t *l) {
    uint32_t ticket = __atomic_fetch_add(&l->next, 1u, __ATOMIC_RELAXED);
    while (__atomic_load_n(&l->owner, __ATOMIC_ACQUIRE) != ticket) {
        __asm__ volatile ("pause" ::: "memory");
    }
}

static inline void spin_unlock(spinlock_t *l) {
    __atomic_fetch_add(&l->owner, 1u, __ATOMIC_RELEASE);
}

/* Bilet almadan dener: yalnizca kilit bossa (owner == next) girer. */
static inline bool spin_trylock(spinlock_t *l) {
    uint32_t t = __atomic_load_n(&l->next, __ATOMIC_RELAXED);
    if (__atomic_load_n(&l->owner, __ATOMIC_ACQUIRE) != t) return false;
    if (!__atomic_compare_exchange_n(&l->next, &t, t + 1u, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        return false;
    __atomic_store_n(&l->owner, t + 1u, __ATOMIC_RELEASE);
    return true;
}

/* ---- kesme durumu ---- */

static inline uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) :: "memory", "cc");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    if (flags & (1ull << 9))            /* IF: yalnizca aciktiysa ac */
        __asm__ volatile ("sti" ::: "memory");
}

static inline void irq_disable(void) {
    __asm__ volatile ("cli" ::: "memory");
}

static inline void irq_enable(void) {
    __asm__ volatile ("sti" ::: "memory");
}

/* ---- atomik makrolar (tam bariyerli) ---- */

#define atomic_add(p, v)        __atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
#define atomic_sub(p, v)        __atomic_fetch_sub((p), (v), __ATOMIC_SEQ_CST)
#define atomic_load(p)          __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define atomic_store(p, v)      __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)
#define atomic_cmpxchg(p, old, new) \
    __atomic_compare_exchange_n((p), (old), (new), false, \
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define atomic_xchg(p, v)       __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)

#endif /* ARCTIAN_SYSTEM_SPINLOCK_H */
