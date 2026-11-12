/* AP baslatma: trampoline blob'unu 0x8000'e koy, INIT-SIPI-SIPI at,
 * 'APOK' bekle. AP burada park edilir (cli + hlt); sched/timer/IDT yok. */
#include "apboot.h"
#include <stddef.h>
#include "cpu.h"
#include "percpu.h"
#include "lapic.h"
#include "clock.h"
#include "serial.h"

_Static_assert(offsetof(ap_params_t, cr3) == 0x00, "cr3 0x8F00'da");
_Static_assert(offsetof(ap_params_t, entry) == 0x08, "entry 0x8F08'de");
_Static_assert(offsetof(ap_params_t, stack_top) == 0x10, "stack_top 0x8F10'da");
_Static_assert(offsetof(ap_params_t, cpu_index) == 0x18, "cpu_index 0x8F18'de");
_Static_assert(offsetof(ap_params_t, apic_id) == 0x1C, "apic_id 0x8F1C'de");
_Static_assert(offsetof(ap_params_t, status) == 0x20, "status 0x8F20'de");

/* Makefile blob'u worktree koku altinda uretir; .incbin yolu da o koka
 * gore (derleme dizini) cozulur. */
__asm__(".section .rodata\n"
        ".balign 16\n"
        ".global ap_trampoline_bin\n"
        "ap_trampoline_bin:\n"
        ".incbin \"build/ap_trampoline.bin\"\n"
        ".global ap_trampoline_end\n"
        "ap_trampoline_end:\n");

extern const uint8_t ap_trampoline_bin[];
extern const uint8_t ap_trampoline_end[];

static uint8_t s_ap_stacks[CPU_MAX_APICS][AP_STACK_SIZE]
    __attribute__((aligned(16)));

static volatile ap_params_t *const s_params =
    (volatile ap_params_t *)(uintptr_t)AP_PARAM_ADDR;

static uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    return v;
}

/* Kesmeler acikken clock_ms ilerler; kapaliysa ilerlemez, bu yuzden bir de
 * sayac siniri var: bekleme uzar ama sonsuza suruklenmez. */
static void ap_delay_ms(uint32_t ms) {
    uint64_t t0 = clock_ms();
    uint64_t spins = 0;
    while ((uint64_t)(clock_ms() - t0) < ms) {
        __asm__ volatile ("pause");
        if (++spins > 200000000ull) break;
    }
}

bool apboot_start(uint32_t cpu_index, uint32_t apic_id) {
    if (cpu_index >= CPU_MAX_APICS) return false;
    if (cpu_index == 0) return false;               /* BSP zaten calisiyor */
    if (lapic_mode() == LAPIC_MODE_NONE) return false;

    size_t blob = (size_t)(ap_trampoline_end - ap_trampoline_bin);
    if (blob == 0 || blob > AP_TRAMP_MAX) return false;

    /* Trampoline 0x8000..0x8FFF bolgesi simetrik gorunur (identity map),
     * cekirdek 0x300000'dan itibaren yuklenir; carpisma yok. */
    volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)AP_TRAMP_ADDR;
    for (size_t i = 0; i < blob; i++) dst[i] = ap_trampoline_bin[i];
    for (size_t i = blob; i < AP_TRAMP_MAX; i++) dst[i] = 0;   /* eski kalinti */
    __asm__ volatile ("wbinvd" ::: "memory");   /* AP cache'ten eski satir okumasun */

    s_params->cr3 = read_cr3();
    s_params->entry = (uint64_t)(uintptr_t)&ap_entry;
    s_params->stack_top = (uint64_t)(uintptr_t)&s_ap_stacks[cpu_index][AP_STACK_SIZE];
    s_params->cpu_index = cpu_index;
    s_params->apic_id = apic_id;
    s_params->status = 0;

    if (!lapic_send_init(apic_id)) return false;
    ap_delay_ms(10);
    if (!lapic_send_sipi(apic_id, AP_SIPI_VECTOR)) return false;
    ap_delay_ms(1);
    if (!lapic_send_sipi(apic_id, AP_SIPI_VECTOR)) return false;

    /* Butce: clock ilerlerse ~100 ms, ilerlemezse 20M pause'ta bir kontrol.
     * Toplam 200M spin asilmaz; boylece kesmeler kapaliyken de doner. */
    uint64_t t0 = clock_ms();
    uint64_t spins = 0;
    for (;;) {
        if (s_params->status == AP_STATUS_APOK) return true;
        if ((uint64_t)(clock_ms() - t0) >= 100u) break;
        if (++spins > 200000000ull) break;
        __asm__ volatile ("pause");
    }
    return s_params->status == AP_STATUS_APOK;
}

void ap_entry(uint32_t cpu_index) {
    percpu_t *p = percpu_by_index(cpu_index);
    if (p) {
        p->online = true;
        p->idle = true;
        p->preempt_count = 0;
        p->spin_locks_held = 0;
        p->current_task = 0;
        serial_printf("AP online index=%u apic=0x%X\n", cpu_index, p->apic_id);
        /* Kayit tamamlanmadan APOK yazmak BSP'yi yaniltir; burada yazilir. */
        s_params->status = AP_STATUS_APOK;
    } else {
        serial_printf("AP bilinmeyen index=%u\n", cpu_index);
    }
    /* Park et: IDT yuklenmedi, kesme beklenmez. */
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}

uint32_t apboot_start_all(void) {
    const cpu_info_t *ci = cpu_info();
    if (!ci->madt_ok) return 0;
    if (lapic_init() && ci->bsp_apic_id != lapic_id()) {
        serial_printf("[apboot] uyari: BSP APIC ID farkli cpuid=%u lapic=0x%X\n",
                      ci->bsp_apic_id, lapic_id());
    }
    uint32_t ok = 0;
    for (uint32_t i = 0; i < ci->apic_count; i++) {
        uint32_t id = ci->apic_ids[i];
        if (id == ci->bsp_apic_id) continue;
        percpu_t *p = percpu_by_apic(id);
        if (!p) continue;
        if (apboot_start(p->index, id)) ok++;
        else serial_printf("[apboot] AP baslamadi apic=0x%X\n", id);
    }
    return ok;
}
