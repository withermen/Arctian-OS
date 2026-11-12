/* Yerel APIC: mod kesfi, ID okuma, EOI ve IPI.
 * Enable/spurious/TPR/timer degistirilmez; bkz. lapic.h. */
#include "lapic.h"
#include "serial.h"
#include "mmio.h"
#include "kernel.h"      /* inb/outb (PIT kanal 2 kalibrasyonu) */

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
 * atlanirsa bazi AP'ler INIT'i almaz. SIPI yalniz assert gonderir. */

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

/* ---- Yerel timer (paket 7) ----
 * Periyodik mod, bolme /16, vektor LAPIC_TIMER_VECTOR (0x40).
 * ONEMLI: vektor 0x40'nin IDT kapisi takilmadan lapic_timer_init()
 * CAGRILMAMALI: kapisi olmayan vektor #GP -> triple fault. Kapi + stub
 * (lapic_timer_irq cagiran) init/idt tarafinin isi; bu paket dokunmaz.
 *
 * Kalibrasyon: PIT kanali 2 tek atim (mode 0) ile ~27.5 ms'lik gercek zaman
 * penceresinde LAPIC sayaci okunur; bus hizi = lapic_tick * 1193182 /
 * 0x7FFF (bolme /16 dahil degil, sayac bolunmus hizda ilerler). PIT sayaci
 * hic hareket etmezse (PIT yok/kilitli) 1 GHz varsayilir (QEMU APIC busi).
 *
 * lapic_timer_irq(): IDT stub'inin cagirecegi kanca. Sira: once EOI, sonra
 * kayitli handler (sched_tick_local). Handler icinde gorev degisimi
 * (switch) olabilir; EOI onceden gonderildigi icin sonraki tick kaclmaz. */

#define XAPIC_SVR     0x0F0u
#define XAPIC_LVT_TIM 0x320u
#define XAPIC_ICOUNT  0x380u
#define XAPIC_CCOUNT  0x390u
#define XAPIC_DCR     0x3E0u

#define MSR_X2APIC_SVR     0x80Fu
#define MSR_X2APIC_LVT_TIM 0x832u
#define MSR_X2APIC_ICOUNT  0x838u
#define MSR_X2APIC_CCOUNT  0x839u
#define MSR_X2APIC_DCR     0x83Eu

#define TMR_PERIODIC   0x00000u        /* LVT bits 17:16 = 00 */
#define TMR_MASKED     (1u << 16)
#define DIV_16         0x3u            /* DCR: kucuk deger 2 << 3 = /16 */
#define SVR_ENABLE     (1u << 8)
#define SVR_SPURIOUS   0xFFu           /* QEMU'da pratikte gelmez */

#define PIT_T2_CMD     0x43u
#define PIT_T2_DATA    0x42u
#define PIT_T2_CNTRL   0x61u
#define PIT_INPUT_HZ   1193182u
#define PIT_T2_COUNT   0x7FFFu         /* ~27.5 ms */

static void (*s_timer_handler)(void);
static uint32_t s_timer_hz;
static volatile bool s_timer_started;

void lapic_timer_set_handler(void (*fn)(void)) { s_timer_handler = fn; }
bool lapic_timer_running(void) { return s_timer_started; }
uint32_t lapic_timer_hz(void) { return s_timer_hz; }

static uint32_t tmr_read(uint32_t xoff, uint32_t msr) {
    if (s_mode == LAPIC_MODE_X2APIC) return (uint32_t)rdmsr(msr);
    return mmio_rd(xoff);
}

static void tmr_write(uint32_t xoff, uint32_t msr, uint32_t v) {
    if (s_mode == LAPIC_MODE_X2APIC) wrmsr(msr, v);
    else mmio_wr(xoff, v);
}

/* PIT kanal 2 tek atim: ~27.5 ms sonra port 0x61 bit 5 kalker.
 * Donus: 1 = pencere tamamlandi, 0 = asiri sarma (PIT calismiyor). */
static bool pit_t2_wait(void) {
    uint8_t old = inb(PIT_T2_CNTRL);
    outb(PIT_T2_CNTRL, old & ~0x03u);            /* gate kapali, out reset */
    outb(PIT_T2_CMD, 0xB0u);                     /* ch2, lo/hi, mode 0 */
    outb(PIT_T2_DATA, (uint8_t)(PIT_T2_COUNT & 0xFFu));
    outb(PIT_T2_DATA, (uint8_t)(PIT_T2_COUNT >> 8));
    outb(PIT_T2_CNTRL, (old & ~0x03u) | 0x01u);  /* gate yuksek -> sayar */
    for (uint32_t i = 0; i < 5000000u; i++) {    /* gercekte ~55k tur */
        if (inb(PIT_T2_CNTRL) & 0x20u) {
            outb(PIT_T2_CNTRL, old);             /* eski kanil konfigurasyon */
            return true;
        }
        __asm__ volatile("pause");
    }
    outb(PIT_T2_CNTRL, old);
    return false;
}

bool lapic_timer_init(uint32_t hz) {
    if (s_mode == LAPIC_MODE_NONE) return false;
    if (hz == 0) hz = 1000;
    if (s_timer_started) return true;            /* BSP bir kez baslatir */

    /* Kesmeler halenuz kapaliysa PIT kalibrasyonu calismaz ama t2 port
     * tabanli olcum IRQ gerektirmez; yine de PIT yoksa 1 GHz fallback. */

    /* SVR: yerel APIC arayuzunu yaz (timer'in ateslenmesi icin sart). */
    tmr_write(XAPIC_SVR, MSR_X2APIC_SVR, SVR_ENABLE | SVR_SPURIOUS);

    /* Bolme /16 (DCR yazimi bir sonraki ICOUNT yaziminda yuklenir). */
    tmr_write(XAPIC_DCR, MSR_X2APIC_DCR, DIV_16);

    /* Maskeli + tam sayac ile kalibrasyon penceresi. */
    tmr_write(XAPIC_LVT_TIM, MSR_X2APIC_LVT_TIM, TMR_MASKED);
    tmr_write(XAPIC_ICOUNT, MSR_X2APIC_ICOUNT, 0xFFFFFFFFu);

    uint32_t start = tmr_read(XAPIC_CCOUNT, MSR_X2APIC_CCOUNT);
    bool measured = pit_t2_wait();
    uint32_t cur = tmr_read(XAPIC_CCOUNT, MSR_X2APIC_CCOUNT);

    uint64_t div_hz;                              /* bus/16 cinsinden */
    if (measured) {
        uint32_t elapsed = start - cur;           /* geriye sayar */
        div_hz = ((uint64_t)elapsed * PIT_INPUT_HZ) / PIT_T2_COUNT;
        if (div_hz == 0) div_hz = 1000000000ull / 16ull;   /* mantikli taban */
    } else {
        div_hz = 1000000000ull / 16ull;           /* QEMU: APIC busi 1 GHz */
    }

    uint64_t initial = div_hz / hz;
    if (initial < 32u) initial = 32u;
    if (initial > 0xFFFFFFFFull) initial = 0xFFFFFFFFull;

    /* Periyodik mod, maskesiz, vektor 0x40; ICOUNT yazimi sayaci baslatir. */
    tmr_write(XAPIC_LVT_TIM, MSR_X2APIC_LVT_TIM, TMR_PERIODIC | LAPIC_TIMER_VECTOR);
    tmr_write(XAPIC_ICOUNT, MSR_X2APIC_ICOUNT, (uint32_t)initial);
    s_timer_hz = hz;
    s_timer_started = true;

    serial_printf("[lapic] timer hz=%u div=%llu initial=%llu (kalibrasyon=%s)\n",
                  hz, (unsigned long long)(div_hz * 16ull),
                  (unsigned long long)initial, measured ? "PIT" : "1GHz-tahmin");
    return true;
}

void lapic_timer_irq(void) {
    lapic_eoi();                     /* once EOI: handler icinde switch olabilir */
    if (s_timer_handler) s_timer_handler();
}
