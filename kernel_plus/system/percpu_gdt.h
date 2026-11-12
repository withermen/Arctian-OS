#ifndef ARCTIAN_SYSTEM_PERCPU_GDT_H
#define ARCTIAN_SYSTEM_PERCPU_GDT_H

#include <stdint.h>
#include "cpu.h"

/* CPU basina GDT + TSS devri.
 *
 * Dosya adi percpu_gdt: main_kernel/gdt.h zaten var ve -Imain_kernel once
 * geldigi icin "gdt.h" o basliga cozulurdu (carpisma).
 *
 * Her CPU kendi GDT kopyasini kullanir (0x00 bos, 0x08 kod, 0x10 veri,
 * 0x18 TSS). TSS yalnizca rsp0 tasir; IST girdileri simdilik sifir cunku
 * idt.c hicbir kapida IST kullanmiyor. iomap_base = sizeof(tss): IO bitmap
 * yok, yani izin kontrolu yok (ring3 henuz yok).
 *
 * gdt_init_cpu() kesme durumunu DEGISTIRMEZ ve IDT icerigine dokunmaz;
 * mevcut IDTR'yi sidt+lidt ile kendi ustune alir (AP'de IDTR reset sonrasi
 * rastlanti oldugu icin bu sart). */

#define GDT_SEL_CODE 0x08u
#define GDT_SEL_DATA 0x10u
#define GDT_SEL_TSS  0x18u

typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1, ist2, ist3, ist4, ist5, ist6, ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed)) tss64_t;

/* BSP'nin sabit yiginin ucu (rsp0 olarak verilir); init.c kullanir. */
extern uint8_t s_bsp_stack[16384];

void gdt_init_cpu(uint32_t index, uint64_t rsp0);
void gdt_set_rsp0(uint32_t index, uint64_t rsp0);

#endif /* ARCTIAN_SYSTEM_PERCPU_GDT_H */
