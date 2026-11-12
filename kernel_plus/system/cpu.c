/* CPU topolojisi: CPUID ve ACPI MADT.
 *
 * Bu paket yalnizca olcer; AP boot, zamanlayici ve sched degismez.
 * MADT yoksa CPUID sayilari raporlanir, madt_ok=false kalir (uydurma yok).
 */
#include "cpu.h"
#include "percpu.h"
#include "serial.h"
#include "mmio.h"

_Static_assert(sizeof(((cpu_info_t *)0)->apic_ids) / sizeof(uint32_t) == CPU_MAX_APICS,
               "apic_ids boyu CPU_MAX_APICS ile uyusmuyor");

static cpu_info_t s_cpu;

static inline void cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4]) {
    __asm__ volatile ("cpuid"
                      : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3])
                      : "a"(leaf), "c"(sub));
}

static int mem_eq(const uint8_t *p, const char *sig, int n) {
    for (int i = 0; i < n; i++) if (p[i] != (uint8_t)sig[i]) return 0;
    return 1;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static int acpi_checksum(const uint8_t *p, uint32_t len) {
    uint8_t s = 0;
    for (uint32_t i = 0; i < len; i++) s = (uint8_t)(s + p[i]);
    return s == 0;
}

/* acpi.c ile ayni kural: 1GB ustu tablo adresleri identity map'lenir. */
static const uint8_t *table_ptr(uint64_t addr, uint64_t size) {
    if (addr >= 0x40000000ull) mmio_map(addr, size ? size : 0x1000u);
    return (const uint8_t *)(uintptr_t)addr;
}

/* RSDP: 0xE0000-0xFFFFF, 16 bayt hizali, ilk 20 baytin checksum'i. */
static const uint8_t *find_rsdp(void) {
    for (uint32_t a = 0xE0000; a < 0x100000; a += 16) {
        const uint8_t *p = (const uint8_t *)(uintptr_t)a;
        if (mem_eq(p, "RSD PTR ", 8) && acpi_checksum(p, 20)) return p;
    }
    return 0;
}

/* XSDT (varsa) veya RSDT icinde imza arar; tablo basini dondurur. */
static const uint8_t *sdt_find(const uint8_t *sdt, const char *sig) {
    if (!sdt) return 0;
    uint32_t len = rd32(sdt + 4);
    if (len < 36 || len > (16u << 20)) return 0;
    int entsize = mem_eq(sdt, "XSDT", 4) ? 8 : 4;
    for (uint32_t off = 36; off + (uint32_t)entsize <= len; off += entsize) {
        uint64_t addr = (entsize == 8) ? rd64(sdt + off) : rd32(sdt + off);
        if (!addr || addr > 0xFFFFFFFFull) continue;
        const uint8_t *t = table_ptr(addr, 0x1000);
        if (mem_eq(t, sig, 4)) {
            uint32_t tl = rd32(t + 4);
            if (tl >= 36 && tl <= (16u << 20)) t = table_ptr(addr, tl);
            return t;
        }
    }
    return 0;
}

static const uint8_t *find_madt(void) {
    const uint8_t *rsdp = find_rsdp();
    if (!rsdp) return 0;
    const uint8_t *sdt = 0;
    uint8_t rev = rsdp[15];
    if (rev >= 2) {
        uint64_t xsdt = rd64(rsdp + 24);
        if (xsdt && xsdt <= 0xFFFFFFFFull) {
            const uint8_t *t = table_ptr(xsdt, 0x1000);
            if (mem_eq(t, "XSDT", 4)) {
                uint32_t tl = rd32(t + 4);
                if (tl >= 36 && tl <= (16u << 20)) t = table_ptr(xsdt, tl);
                if (acpi_checksum(t, rd32(t + 4))) sdt = t;
            }
        }
    }
    if (!sdt) {
        uint32_t rsdt = rd32(rsdp + 16);   /* RSDT her zaman 32-bit adres tasi */
        if (rsdt) {
            const uint8_t *t = table_ptr(rsdt, 0x1000);
            if (mem_eq(t, "RSDT", 4)) {
                uint32_t tl = rd32(t + 4);
                if (tl >= 36 && tl <= (16u << 20)) t = table_ptr(rsdt, tl);
                if (acpi_checksum(t, rd32(t + 4))) sdt = t;
            }
        }
    }
    return sdt_find(sdt, "APIC");
}

/* Ayni APIC ID'yi iki kez yazma (type 0 ve type 9 cakisabilir). */
static void add_apic_id(uint32_t id) {
    for (uint32_t i = 0; i < s_cpu.apic_count; i++)
        if (s_cpu.apic_ids[i] == id) return;
    if (s_cpu.apic_count < CPU_MAX_APICS)
        s_cpu.apic_ids[s_cpu.apic_count++] = id;
}

/* MADT: type 0 LAPIC ve type 9 x2APIC (ikisinde de etkin = flags bit 0). */
static void scan_madt(const uint8_t *madt) {
    uint32_t len = rd32(madt + 4);
    if (len < 44 || len > (16u << 20)) return;
    s_cpu.madt_ok = true;
    for (uint32_t off = 44; off + 2 <= len; ) {
        uint8_t type = madt[off];
        uint8_t elen = madt[off + 1];
        if (elen < 2 || off + elen > len) break;
        if (type == 0 && elen >= 8) {
            uint32_t flags = rd32(madt + off + 4);
            if (flags & 1u) add_apic_id(madt[off + 3]);
        } else if (type == 9) {
            s_cpu.x2apic = true;
            if (elen >= 12) {
                uint32_t flags = rd32(madt + off + 8);
                if (flags & 1u) add_apic_id(rd32(madt + off + 4));
            }
        }
        off += elen;
    }
}

/* CPUID leaf 0x1F (yoksa 0x0B): SMT ve core seviyelerinden topoloji. */
static void detect_topology(uint32_t logical, uint32_t max_leaf) {
    uint32_t leaf = 0;
    if (max_leaf >= 0x1F) leaf = 0x1F;
    else if (max_leaf >= 0x0B) leaf = 0x0B;

    uint32_t smt = 0;       /* thread / cekirdek */
    uint32_t per_pkg = 0;   /* thread / paket (core seviyesi sayaci) */
    if (leaf) {
        for (uint32_t sub = 0; sub < 32; sub++) {
            uint32_t r[4];
            cpuid(leaf, sub, r);
            uint32_t type = (r[2] >> 8) & 0xFFu;
            uint32_t count = r[1] & 0xFFFFu;
            if (count == 0) break;
            if (type == 1) smt = count;
            else if (type == 2) per_pkg = count;
        }
    }
    if (!per_pkg) per_pkg = logical ? logical : 1;
    if (!smt) smt = 1;

    s_cpu.packages = per_pkg ? logical / per_pkg : 1;
    if (!s_cpu.packages) s_cpu.packages = 1;
    uint32_t cores_pp = per_pkg / smt;
    if (!cores_pp) cores_pp = 1;
    s_cpu.cores = cores_pp * s_cpu.packages;
}

void cpu_detect(void) {
    for (unsigned i = 0; i < sizeof(s_cpu); i++) ((uint8_t *)&s_cpu)[i] = 0;

    uint32_t r[4];
    cpuid(0, 0, r);
    uint32_t max_leaf = r[0];

    cpuid(1, 0, r);
    uint32_t logical = (r[1] >> 16) & 0xFFu;
    s_cpu.bsp_apic_id = (r[1] >> 24) & 0xFFu;
    if (!logical) logical = 1;
    s_cpu.logical = logical;

    detect_topology(logical, max_leaf);

    const uint8_t *madt = find_madt();
    if (madt) scan_madt(madt);
}

const cpu_info_t *cpu_info(void) {
    return &s_cpu;
}

bool cpu_smp_ready(void) {
    /* AP boot apboot_start_all() ile gerceklestiginde online sayisi 1'i gecer;
     * init.c o cagriyi yapmadigi surece bu false kalir. */
    return percpu_online_count() > 1;
}

void cpu_dump(void) {
    serial_printf("[cpu] cekirdek=%u thread=%u paket=%u apic=%u madt=%s x2apic=%s\n",
                  s_cpu.cores, s_cpu.logical, s_cpu.packages, s_cpu.bsp_apic_id,
                  s_cpu.madt_ok ? "var" : "yok",
                  s_cpu.x2apic ? "var" : "yok");
}
