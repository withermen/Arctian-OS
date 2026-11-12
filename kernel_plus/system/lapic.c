/* Yerel APIC: mod kesfi, ID okuma, EOI ve IPI.
 * Enable/spurious/TPR/timer degistirilmez; bkz. lapic.h. */
#include "lapic.h"
#include "serial.h"
#include "mmio.h"

#define MSR_APIC_BASE 0x1Bu
#define MSR_APIC_ID   0x802u
#define MSR_APIC_TPR  0x808u
#define MSR_APIC_EOI  0x80Bu
#define MSR_APIC_ICR  0x830u

#define APIC_BASE_MASK 0x000FFFFFFFFFF000ull
#define APIC_BASE_X2    (1ull << 10)
#define APIC_BASE_EN    (1ull << 11)

/* xAPIC MMIO kayit ofsetleri */
#define XAPIC_ID    0x020u
#define XAPIC_TPR   0x080u
#define XAPIC_EOI   0x0B0u
#define XAPIC_ICR_LO 0x300u
#define XAPIC_ICR_HI 0x310u

#define ICR_DELIVERY_STATUS (1u << 12)
#define ICR_SHORTHAND_ALL_EXCL_SELF (3u << 18)

#define ICR_WAIT_SPINS 1000000u   /* ~birkac ms; sahte deger degil, ust sinir */

static lapic_mode_t s_mode;
static uint64_t     s_base;
static volatile uint32_t *s_mmio;
static uint32_t     s_id;

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile ("wrmsr"
                      : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32))
                      : "memory");
}

static inline void mmio_wr(uint32_t off, uint32_t v) {
    s_mmio[off / 4] = v;
}

static inline uint32_t mmio_rd(uint32_t off) {
    return s_mmio[off / 4];
}

bool lapic_init(void) {
    s_mode = LAPIC_MODE_NONE;
    s_base = 0;
    s_mmio = 0;
    s_id = 0;

    uint64_t apic_base = rdmsr(MSR_APIC_BASE);
    s_base = apic_base & APIC_BASE_MASK;

    /* Enable (bit 11) kapaliysa LAPIC calismiyor demektir; uydurma mod
     * raporlamak yerine basarisiz say. */
    if (!(apic_base & APIC_BASE_EN)) return false;

    if (apic_base & APIC_BASE_X2) {
        /* x2APIC: MMIO'ya dokunmadan MSR'den kimlik. */
        s_mode = LAPIC_MODE_X2APIC;
        s_id = (uint32_t)(rdmsr(MSR_APIC_ID) & 0xFFFFFFFFu);
        return true;
    }

    /* xAPIC: taban tipik 0xFEE00000, 1GB ustu -> identity map sart. */
    if (!s_base) return false;
    if (mmio_map(s_base, 0x1000u) != 0) return false;
    s_mmio = (volatile uint32_t *)(uintptr_t)s_base;
    s_mode = LAPIC_MODE_XAPIC;
    s_id = (mmio_rd(XAPIC_ID) >> 24) & 0xFFu;
    return true;
}

lapic_mode_t lapic_mode(void) { return s_mode; }
uint64_t     lapic_base(void) { return s_base; }
uint32_t     lapic_id(void)   { return s_id; }

void lapic_eoi(void) {
    if (s_mode == LAPIC_MODE_X2APIC) wrmsr(MSR_APIC_EOI, 0);
    else if (s_mode == LAPIC_MODE_XAPIC) mmio_wr(XAPIC_EOI, 0);
}

/* xAPIC'te bir sonraki yazmadan once delivery status (bit 12) temiz olmali.
 * Asiri beklersek yazmayi birakip false doneriz; sonsuz donguye girmeyiz. */
static bool icr_wait_idle(void) {
    for (uint32_t i = 0; i < ICR_WAIT_SPINS; i++) {
        if (!(mmio_rd(XAPIC_ICR_LO) & ICR_DELIVERY_STATUS)) return true;
    }
    return false;
}

/* x2APIC'te ayni kural MSR 0x830 bit 12 icin gecerli. */
static bool icr_wait_idle_x2apic(void) {
    for (uint32_t i = 0; i < ICR_WAIT_SPINS; i++) {
        if (!(rdmsr(MSR_APIC_ICR) & ICR_DELIVERY_STATUS)) return true;
    }
    return false;
}

bool lapic_send_ipi(uint32_t dest_apic_id, uint8_t vector) {
    if (!vector) return false;
    if (s_mode == LAPIC_MODE_X2APIC) {
        if (!icr_wait_idle_x2apic()) return false;
        uint64_t icr = ((uint64_t)(dest_apic_id & 0xFFFFFFFFu) << 32) | vector;
        wrmsr(MSR_APIC_ICR, icr);
        return true;
    }
    if (s_mode != LAPIC_MODE_XAPIC) return false;
    if (!icr_wait_idle()) return false;
    mmio_wr(XAPIC_ICR_HI, (dest_apic_id & 0xFFu) << 24);
    mmio_wr(XAPIC_ICR_LO, vector);          /* fixed delivery, physical dest */
    return true;
}

bool lapic_send_ipi_all_excl_self(uint8_t vector) {
    if (!vector) return false;
    if (s_mode == LAPIC_MODE_X2APIC) {
        if (!icr_wait_idle_x2apic()) return false;
        wrmsr(MSR_APIC_ICR, ICR_SHORTHAND_ALL_EXCL_SELF | vector);
        return true;
    }
    if (s_mode != LAPIC_MODE_XAPIC) return false;
    if (!icr_wait_idle()) return false;
    mmio_wr(XAPIC_ICR_LO, ICR_SHORTHAND_ALL_EXCL_SELF | vector);
    return true;
}

/* ---- INIT / SIPI (AP boot) ----
 * Delivery mode: INIT = 5, SIPI = 6. Linux ile ayni siralama:
 *   INIT assert   = DM_INIT | 0x4000 (bit 14)    -> 0x4500
 *   INIT deassert = DM_INIT            (bit 14/15 temiz) -> 0x0500
 * x2APIC'te level biti yok sayilir ama ayni iki yazim yapilir; AI yazim
 * atlanirsa bazi AP'ler INIT'i almaz. SIPI yalnız assert gonderir. */

#define ICR_DM_INIT   (5u << 8)
#define ICR_DM_SIPI   (6u << 8)
#define ICR_ASSERT    0x4000u

bool lapic_send_init(uint32_t dest_apic_id) {
    uint32_t assert_v  = ICR_DM_INIT | ICR_ASSERT;   /* 0x4500 */
    uint32_t deassert_v = ICR_DM_INIT;               /* 0x0500 */

    if (s_mode == LAPIC_MODE_X2APIC) {
        if (!icr_wait_idle_x2apic()) return false;
        wrmsr(MSR_APIC_ICR, ((uint64_t)dest_apic_id << 32) | assert_v);
        if (!icr_wait_idle_x2apic()) return false;
        wrmsr(MSR_APIC_ICR, ((uint64_t)dest_apic_id << 32) | deassert_v);
        return true;
    }
    if (s_mode != LAPIC_MODE_XAPIC) return false;
    mmio_wr(XAPIC_ICR_HI, (dest_apic_id & 0xFFu) << 24);
    if (!icr_wait_idle()) return false;
    mmio_wr(XAPIC_ICR_LO, assert_v);                 /* assert */
    if (!icr_wait_idle()) return false;
    mmio_wr(XAPIC_ICR_LO, deassert_v);               /* deassert */
    return true;
}

bool lapic_send_sipi(uint32_t dest_apic_id, uint8_t vector) {
    if (!vector) return false;
    if (s_mode == LAPIC_MODE_X2APIC) {
        if (!icr_wait_idle_x2apic()) return false;
        wrmsr(MSR_APIC_ICR, ((uint64_t)dest_apic_id << 32) | ICR_DM_SIPI |
                            ICR_ASSERT | vector);
        return true;
    }
    if (s_mode != LAPIC_MODE_XAPIC) return false;
    mmio_wr(XAPIC_ICR_HI, (dest_apic_id & 0xFFu) << 24);
    if (!icr_wait_idle()) return false;
    mmio_wr(XAPIC_ICR_LO, ICR_DM_SIPI | ICR_ASSERT | vector);      /* assert */
    return true;
}
