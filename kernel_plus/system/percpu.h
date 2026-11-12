#ifndef ARCTIAN_SYSTEM_PERCPU_H
#define ARCTIAN_SYSTEM_PERCPU_H

#include <stdint.h>
#include <stdbool.h>
#include "cpu.h"

/* CPU basina veri. Dizi sabit (CPU_MAX_APICS); AP'ler geldikce ayni
 * girdiler online olur, yeni bellek ayrilmaz.
 *
 * Kilit sirasi kurali: percpu -> runqueue -> timer.
 * percpu alanlarina erisirken once kendi girdini kilitle; baska CPU'nun
 * girdisini tutarken runqueue/timer kilidi alma (ters sira deadlock). */

typedef struct {
    uint32_t apic_id;
    uint32_t index;             /* s_cpu[] icindeki konum, 0 = BSP */
    bool     online;            /* AP boot tamamlandi mi */
    bool     idle;              /* su an bos dongude mi */
    void    *current_task;      /* calisan gorev (sched baglaninca dolar) */
    uint32_t preempt_count;     /* >0 iken preemption kapali */
    uint32_t spin_locks_held;   /* bu CPU'nun tuttugu kilit sayisi */
} percpu_t;

void      percpu_init(void);                    /* BSP'yi kaydeder */
percpu_t *percpu_by_apic(uint32_t apic_id);
percpu_t *percpu_by_index(uint32_t index);
percpu_t *percpu_self(void);                    /* bilinmiyorsa NULL */
uint32_t  percpu_self_index(void);              /* BSP'de 0, bilinmiyorsa UINT32_MAX */
uint32_t  percpu_online_count(void);
uint32_t  percpu_count(void);                   /* kayitli girdi sayisi */

#endif /* ARCTIAN_SYSTEM_PERCPU_H */
