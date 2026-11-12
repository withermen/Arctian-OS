/* CPU basina veri. Bu pakette yalnizca BSP kaydedilir; MADT'te gorulen
 * diger islemciler cevrimdisi girdi olarak eklenir (AP boot yok). */
#include "percpu.h"

_Static_assert(CPU_MAX_APICS > 0, "CPU_MAX_APICS sifir olamaz");

static percpu_t s_cpu[CPU_MAX_APICS];
static uint32_t  s_count;

static inline void cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4]) {
    __asm__ volatile ("cpuid"
                      : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3])
                      : "a"(leaf), "c"(sub));
}

static uint32_t current_apic_id(void) {
    uint32_t r[4];
    cpuid(0, 0, r);
    uint32_t max_leaf = r[0];

    /* x2APIC kimligi: once 0x1F, sonra 0x0B (ikisinde de subleaf 1 EDX).
     * EDX sifirsa gecerli bir numara yok demektir, siradaki yola dus. */
    if (max_leaf >= 0x1F) {
        cpuid(0x1F, 1, r);
        if (r[3]) return r[3];
    }
    if (max_leaf >= 0x0B) {
        cpuid(0x0B, 1, r);
        if (r[3]) return r[3];
    }
    cpuid(1, 0, r);
    return (r[1] >> 24) & 0xFFu;
}

static percpu_t *add_entry(uint32_t apic_id, bool online) {
    if (s_count >= CPU_MAX_APICS) return 0;
    percpu_t *p = &s_cpu[s_count];
    p->apic_id = apic_id;
    p->index = s_count++;
    p->online = online;
    p->idle = online;
    p->current_task = 0;
    p->preempt_count = 0;
    p->spin_locks_held = 0;
    return p;
}

void percpu_init(void) {
    for (uint32_t i = 0; i < CPU_MAX_APICS; i++) {
        s_cpu[i].apic_id = 0;
        s_cpu[i].index = 0;
        s_cpu[i].online = false;
        s_cpu[i].idle = false;
        s_cpu[i].current_task = 0;
        s_cpu[i].preempt_count = 0;
        s_cpu[i].spin_locks_held = 0;
    }
    s_count = 0;

    const cpu_info_t *ci = cpu_info();
    add_entry(ci->bsp_apic_id, true);       /* index 0 = BSP */

    /* MADT'ten bilinen digerleri: cevrimdisi, kimlikleri hazir. */
    for (uint32_t i = 0; i < ci->apic_count; i++) {
        uint32_t id = ci->apic_ids[i];
        if (id == ci->bsp_apic_id) continue;
        if (percpu_by_apic(id)) continue;
        add_entry(id, false);
    }
}

percpu_t *percpu_by_apic(uint32_t apic_id) {
    for (uint32_t i = 0; i < s_count; i++)
        if (s_cpu[i].apic_id == apic_id) return &s_cpu[i];
    return 0;
}

percpu_t *percpu_by_index(uint32_t index) {
    if (index >= s_count) return 0;
    return &s_cpu[index];
}

percpu_t *percpu_self(void) {
    /* Bilinmeyen APIC ID'de BSP'ye dusmuyoruz: cagiran acikca karar versin.
     * Sessiz dusme, yeni bir AP'nin BSP verisini ezmesine yol acabilir. */
    return percpu_by_apic(current_apic_id());
}

uint32_t percpu_self_index(void) {
    percpu_t *p = percpu_self();
    /* NULL'da 0 dondurmek BSP ile karisir; UINT32_MAX "bilinmiyor". */
    return p ? p->index : UINT32_MAX;
}

uint32_t percpu_online_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < s_count; i++) if (s_cpu[i].online) n++;
    return n;
}

uint32_t percpu_count(void) {
    return s_count;
}
