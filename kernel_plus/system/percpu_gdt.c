/* CPU basina GDT + TSS. bkz. percpu_gdt.h. */
#include "percpu_gdt.h"
#include "serial.h"

_Static_assert(sizeof(tss64_t) == 104, "tss64_t 104 bayt olmali");

#define GDT_CODE64 0x00AF9A000000FFFFull   /* base 0, limit FFFFF, L=1 D=0 */
#define GDT_DATA64 0x00CF92000000FFFFull   /* base 0, limit FFFFF, D=1 */

typedef struct {
    uint64_t null;
    uint64_t code;
    uint64_t data;
    uint64_t tss_low;     /* 0x18: 16 baytlik sistem tanimi */
    uint64_t tss_high;
} gdt64_t;

_Static_assert(sizeof(gdt64_t) == 40, "gdt64_t 40 bayt olmali");

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) dtable_ptr_t;

uint8_t s_bsp_stack[16384] __attribute__((aligned(16)));

static tss64_t  s_tss[CPU_MAX_APICS] __attribute__((aligned(16)));
static gdt64_t  s_gdt[CPU_MAX_APICS] __attribute__((aligned(16)));

static void tss_desc_set(gdt64_t *g, uint64_t base, uint32_t limit) {
    uint64_t lo = (uint64_t)(limit & 0xFFFFu)
                | ((base & 0x00FFFFFFull) << 16)          /* base[23:0] */
                | (((base >> 24) & 0xFFu) << 32)          /* base[31:24] */
                | ((uint64_t)0x89u << 40)                 /* var, 64-bit TSS */
                | ((uint64_t)((limit >> 16) & 0xFu) << 48)
                | ((uint64_t)0x0u << 52)                  /* G/D/L/AVL = 0 */
                | (((base >> 32) & 0xFFu) << 56);         /* byte7 */
    g->tss_low = lo;
    g->tss_high = base >> 32;                             /* base[63:32] */
}

void gdt_set_rsp0(uint32_t index, uint64_t rsp0) {
    if (index >= CPU_MAX_APICS) return;
    s_tss[index].rsp0 = rsp0;
}

void gdt_init_cpu(uint32_t index, uint64_t rsp0) {
    if (index >= CPU_MAX_APICS) return;

    tss64_t *t = &s_tss[index];
    uint8_t *raw = (uint8_t *)t;
    for (uint32_t i = 0; i < sizeof(*t); i++) raw[i] = 0;
    t->rsp0 = rsp0;
    t->iomap_base = (uint16_t)sizeof(tss64_t);    /* IO bitmap yok */

    gdt64_t *g = &s_gdt[index];
    g->null = 0;
    g->code = GDT_CODE64;
    g->data = GDT_DATA64;
    tss_desc_set(g, (uint64_t)(uintptr_t)t, (uint32_t)sizeof(tss64_t) - 1u);

    dtable_ptr_t gp = { (uint16_t)(sizeof(*g) - 1u), (uint64_t)(uintptr_t)g };
    __asm__ volatile ("lgdt %0" :: "m"(gp) : "memory");

    __asm__ volatile (
        "mov %0, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%ss\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        :
        : "r"((uint16_t)GDT_SEL_DATA)
        : "ax", "memory");

    __asm__ volatile ("ltr %0" :: "r"((uint16_t)GDT_SEL_TSS) : "memory");

    /* CS'yi far return ile tazele: gizli tanim onbellegi yeni GDT'den okunsun. */
    __asm__ volatile (
        "push %0\n\t"
        "lea 1f(%%rip), %%rax\n\t"
        "push %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        :
        : "n"((uint16_t)GDT_SEL_CODE)
        : "rax", "memory");

    /* IDTR CPU basina bir kayittir; AP'de reset sonrasi deger rastlanti.
     * Mevcut IDT'yi oldugu gibi kendi CPU'muza aliyoruz (icerik degismez). */
    dtable_ptr_t ip;
    __asm__ volatile ("sidt %0" : "=m"(ip));
    __asm__ volatile ("lidt %0" :: "m"(ip) : "memory");
}
