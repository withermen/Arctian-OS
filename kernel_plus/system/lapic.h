#ifndef ARCTIAN_SYSTEM_LAPIC_H
#define ARCTIAN_SYSTEM_LAPIC_H

#include <stdint.h>
#include <stdbool.h>

/* Yerel APIC kesfi ve IPI gonderme.
 *
 * Bu pakette yalnizca mod/taban kesfedilir ve ID okunur. Enable, spurious
 * vector, TPR, timer ve IDT DEGISTIRILMEZ; kesme acilmaz. Bu yuzden
 * lapic_init() cagrilmadan da diger islevler guvenli sekilde hata verir.
 *
 * Iki mod:
 *   xAPIC  - MMIO (tipik 0xFEE00000), 32-bit kayitlar
 *   x2APIC - MSR (0x802 ID, 0x80B EOI, 0x830 ICR, 0x808 TPR) */

typedef enum {
    LAPIC_MODE_NONE = 0,
    LAPIC_MODE_XAPIC,
    LAPIC_MODE_X2APIC
} lapic_mode_t;

bool         lapic_init(void);          /* mod + taban + ID kesfi; 1 = basarili */
lapic_mode_t lapic_mode(void);
uint64_t     lapic_base(void);          /* MMIO taban (xAPIC) */
uint32_t     lapic_id(void);

void lapic_eoi(void);
bool lapic_send_ipi(uint32_t dest_apic_id, uint8_t vector);
bool lapic_send_ipi_all_excl_self(uint8_t vector);
bool lapic_send_init(uint32_t dest_apic_id);          /* INIT deassert dahil */
bool lapic_send_sipi(uint32_t dest_apic_id, uint8_t vector);

/* ---- Yerel timer (paket 7 + C3) ----
 * Periyodik, bolme /16, vektor 0x40. Kalibrasyon BSP'de bir kez
 * (lapic_timer_calibrate); her CPU kendi sayacini lapic_timer_start_cpu()
 * ile kurar - LVT/ICOUNT CPU-yerel kayitlar oldugu icin AP'nin timer'i
 * kendi uzerinde baslatilir. DIKKAT: vektor 0x40 icin IDT kapisi + stub
 * (lapic_timer_irq cagiran) takilmadan timer baslatilmamali; kapisi
 * olmayan vektor #GP -> triple fault. AP'de start_cpu'dan once IDT
 * yuklenmis (lidt) olmali. lapic_timer_irq() once EOI sonra kayitli
 * handler'i (sched_tick_local) cagirir. */

#define LAPIC_TIMER_VECTOR 0x40u
#define LAPIC_RESCHED_VECTOR 0x41u    /* C3: yeniden secim istegi IPI */
#define LAPIC_TLB_VECTOR     0x42u    /* C3: TLB shootdown IPI */

bool     lapic_cpu_enable(void);            /* bu CPU: APIC_BASE EN + SVR */
void     lapic_ap_mask_lint(void);          /* AP: LINT0/1 maskesi (PIC kes) */
bool     lapic_timer_calibrate(uint32_t hz);/* BSP: olc, degeri sakla */
bool     lapic_timer_start_cpu(void);       /* bu CPU: periyodik timeri ac */
bool     lapic_timer_init(uint32_t hz);     /* kalibrate + start (BSP tek CPU) */
void     lapic_timer_irq(void);             /* 0x40 IDT stub'unun kancasi */
void     lapic_timer_set_handler(void (*fn)(void));
bool     lapic_timer_running(void);
uint32_t lapic_timer_hz(void);

#endif /* ARCTIAN_SYSTEM_LAPIC_H */
