#ifndef ARCTIAN_SCHED_H
#define ARCTIAN_SCHED_H

#include <stdint.h>

/* C4: sablon sabit dizi yerine dinamik havuz. s_tasks havuzu boot'ta bir
 * kez SCHED_TASK_HARD_CAP kadar ayrilir (heap'ten); gercek kapasite
 * (kabul edilen gorev sayisi tavanı) calisma zamaninda
 * min(64 x cevrimici CPU, HARD_CAP, RAM politikasi) ile belirlenir ve
 * sched_capacity() ile okunur. Tek CPU geri uyumu: kapasite 64. */
#define SCHED_TASK_HARD_CAP 4096
#define SCHED_CAP_PER_CPU   64u

typedef int sched_tid_t;

typedef enum {
    TASK_UNUSED = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_DONE,
    TASK_CARRY          /* C3 is calma tasima hali: hedef CPU sahiplendi,
                         * henuz kuyruguna girmedi; seciciler gormez */
} sched_state_t;

void        sched_init(void);
sched_tid_t sched_spawn(const char *name, void (*fn)(void *), void *arg);
sched_tid_t sched_spawn_ex(const char *name, void (*fn)(void *), void *arg, uint64_t cr3);
sched_tid_t sched_spawn_stack(const char *name, void (*fn)(void *), void *arg,
                              uint64_t cr3, void *stack, uint32_t stack_size);
int         sched_state(sched_tid_t tid);
uint64_t    sched_current_cr3(void);
void        sched_yield(void);
void        sched_sleep_ms(uint32_t ms);
void        sched_exit(void);
void        sched_tick(void);
sched_tid_t sched_current(void);
int         sched_task_count(void);
uint32_t    sched_task_ticks(sched_tid_t tid);
uint32_t    sched_total_ticks(void);
int         sched_selftest(void);

void        sched_prepare_block(void);
void        sched_commit_block(void);
void        sched_block_current(void);
void        sched_wake(sched_tid_t tid);
void        sched_reap(sched_tid_t tid);

/* Paket 7: CPU basina run queue.
 * sched_enqueue_cpu: gorevi o CPU'nun kuyruguna tasir (READY ise kuyruga
 * girer). sched_pick_local: bu CPU'nun kuyrugundan READY gorev secip
 * switch eder; bos ise -1 doner. CPU 0'da legacy round-robin'e yonlendirir
 * (BSP davranisi degismez). sched_tick_local: LAPIC timer kancasi
 * (lapic_timer_irq cagirir; CPU 0'da legacy sched_tick'e devreder). */
int         sched_enqueue_cpu(uint32_t cpu, sched_tid_t tid);
sched_tid_t sched_pick_local(void);
void        sched_tick_local(void);

/* C3 SMP IPI handler'lari (isr.c vektor 0x41/0x42 dagitimi; zayif karsi
 * taraf). smp_tlb_shootdown_all: paging.c vmm_unmap'tan zayif bildiriminle
 * cagrilir; tum diger cevrimici CPU'larda CR3 yenileme IPI'si atar. */
void        smp_resched_irq(void);
void        smp_tlb_irq(void);
void        smp_tlb_shootdown_all(void);

void        sched_set_owner(sched_tid_t tid, void *owner);
void       *sched_owner(sched_tid_t tid);
void       *sched_owner_current(void);

uint64_t    sched_irq_save(void);
void        sched_irq_restore(uint64_t flags);

/* C4 kapasite sozlesmesi: sched_capacity kabul tavanini, sched_task_count
 * kullanimi dondurur (gorev yoneticisi "kullanilan / kapasite" gosterir).
 * sched_scale_capacity: AP bootu sonrasi cevrimici CPU sayisina gore
 * tavani yeniden hesaplar (yalniz buyutur, kucultmez). */
int         sched_capacity(void);
void        sched_scale_capacity(void);

#endif
