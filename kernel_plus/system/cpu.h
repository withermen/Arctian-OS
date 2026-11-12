#ifndef ARCTIAN_SYSTEM_CPU_H
#define ARCTIAN_SYSTEM_CPU_H

#include <stdint.h>
#include <stdbool.h>

/* Derleme zamani sinir: cpu_info_t.apic_ids ile ayni olmali. */
#define CPU_MAX_APICS 64

typedef struct {
    uint32_t logical;           /* toplam mantiksal islemci (CPUID leaf 1) */
    uint32_t cores;             /* toplam fiziksel cekirdek */
    uint32_t packages;          /* fiziksel paket (socket) sayisi */
    uint32_t bsp_apic_id;       /* boot islemcisinin APIC ID'si */
    uint32_t apic_ids[CPU_MAX_APICS]; /* MADT type 0 etkin APIC ID'leri */
    uint32_t apic_count;
    bool     madt_ok;           /* MADT bulundu ve ayristirildi */
    bool     x2apic;            /* MADT type 9 girdisi var */
} cpu_info_t;

void              cpu_detect(void);
const cpu_info_t *cpu_info(void);
void              cpu_dump(void);
bool              cpu_smp_ready(void); /* bu pakette daima false */

#endif /* ARCTIAN_SYSTEM_CPU_H */
