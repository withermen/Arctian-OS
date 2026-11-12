#ifndef ARCTIAN_SYSTEM_APBOOT_H
#define ARCTIAN_SYSTEM_APBOOT_H

#include <stdint.h>
#include <stdbool.h>

/* AP (Application Processor) baslatma.
 *
 * Akis: trampoline blob'u fiziksel 0x8000'e kopyalanir, parametre bloku
 * 0x8F00'a yazilir, INIT + SIPI(0x08) + SIPI(0x08) gonderilir. AP 16->32->64
 * gecisini trampoline'de yapar ve status'a 'AP64' yazar; ap_entry percpu
 * kaydini online yaptiktan SONRA 'APOK' yazar. BSP yalniz 'APOK'u basari
 * saysin (AP64 tek basina kayit tamamlanmamis demek). AP burada cli + hlt
 * dongusunde park edilir; IDT yuklemez, zamanlayici veya sched calistirmaz.
 * BSP butceli bekler (clock varsa ~100 ms, yoksa 200M spin).
 *
 * Bu dosya init.c'den cagrilmaz; cagri yeri paket 5'te kararlastrilacak. */

#define AP_TRAMP_ADDR   0x8000u          /* trampoline'in kopyalandigi yer */
#define AP_PARAM_ADDR   0x8F00u          /* parametre bloku */
#define AP_TRAMP_MAX    0xF00u           /* 0x8000..0x8F00 arasi */
#define AP_SIPI_VECTOR  0x08u            /* 0x0800:0000 = 0x8000 */
#define AP_STATUS_AP64  0x34365041u      /* 'AP64' trampoline mod gecisi yazdi */
#define AP_STATUS_APOK  0x4B4F5041u      /* 'APOK' ap_entry kaydi tamamladi */
#define AP_STACK_SIZE   16384u           /* AP basina yigin */

/* ap_trampoline.asm ile birebir ayni olmali. */
typedef struct {
    uint64_t cr3;
    uint64_t entry;
    uint64_t stack_top;
    uint32_t cpu_index;
    uint32_t apic_id;
    uint32_t status;
} ap_params_t;

bool     apboot_start(uint32_t cpu_index, uint32_t apic_id);
uint32_t apboot_start_all(void);         /* BSP disindakileri baslat */
void     ap_entry(uint32_t cpu_index);   /* AP'nin C girisi (nasm'dan) */

#endif /* ARCTIAN_SYSTEM_APBOOT_H */
