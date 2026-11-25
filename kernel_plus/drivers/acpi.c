#include "acpi.h"
#include "pci.h"
#include "kernel.h"
#include "serial.h"
#include "mmio.h"
#include "paging.h"
#include <stddef.h>

acpi_info_t g_acpi;

/* ------------------------------------------------------------------ */
/* Tablo kesfi                                                        */
/* ------------------------------------------------------------------ */

static int mem_eq(const uint8_t *p, const char *sig, int n) {
    for (int i = 0; i < n; i++) if (p[i] != (uint8_t)sig[i]) return 0;
    return 1;
}

static int acpi_checksum(const uint8_t *p, uint32_t len) {
    uint8_t s = 0;
    for (uint32_t i = 0; i < len; i++) s = (uint8_t)(s + p[i]);
    return s == 0;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* 1GB ustundeki fiziksel ACPI tablo adreslerini (>4 GB dahil) 2MB sayfalarla
 * identity map'ler. VMM hazirsa vmm_map_mmio_range() kullanilir; degilse
 * mmio_map() yedegine dusulur. */
static const uint8_t *acpi_ensure(uint64_t addr, uint32_t size) {
    if (!addr) return 0;
    uint32_t sz = size ? size : 0x1000u;
    if (addr + sz < addr) return 0;
    if (addr >= 0x40000000ull || addr + sz > 0x40000000ull) {
        if (vmm_map_mmio_range(addr, sz) != 0)
            mmio_map(addr, sz);
    }
    return (const uint8_t *)(uintptr_t)addr;
}

/* RSDP: UEFI yolunda 0x7100 (arctian_uefiinfo_t), BIOS yolunda EBDA ve
 * 0xE0000-0xFFFFF araliginda aranir. >4 GB fiziksel adres desteklenir. */
#define ACPI_UEFIINFO_ADDR  0x7100u
#define ACPI_UEFIINFO_MAGIC 0x49464555u
static const uint8_t *find_rsdp(void) {
    const uint32_t *ui = (const uint32_t *)(uintptr_t)ACPI_UEFIINFO_ADDR;
    if (ui[0] == ACPI_UEFIINFO_MAGIC) {
        uint64_t r = *(const uint64_t *)(uintptr_t)(ACPI_UEFIINFO_ADDR + 0x28u);
        if (r) {
            const uint8_t *p = acpi_ensure(r, 64);
            if (p && mem_eq(p, "RSD PTR ", 8) && acpi_checksum(p, 20)) return p;
        }
    }
    uint16_t ebda;
    __asm__ volatile ("movw 0x40E, %0" : "=r"(ebda));
    uint32_t base = (uint32_t)ebda << 4;
    if (base >= 0x400 && base < 0xA0000) {
        for (uint32_t a = base; a < base + 1024; a += 16) {
            const uint8_t *p = (const uint8_t *)(uintptr_t)a;
            if (mem_eq(p, "RSD PTR ", 8) && acpi_checksum(p, 20)) return p;
        }
    }
    for (uint32_t a = 0xE0000; a < 0x100000; a += 16) {
        const uint8_t *p = (const uint8_t *)(uintptr_t)a;
        if (mem_eq(p, "RSD PTR ", 8) && acpi_checksum(p, 20)) return p;
    }
    return 0;
}

/* Verilen tablo imzasini RSDT/XSDT icinde arar (tam 64-bit adres destegi). */
static const uint8_t *sdt_find(const uint8_t *sdt, const char *sig) {
    if (!sdt) return 0;
    uint32_t len = rd32(sdt + 4);
    if (len < 36 || len > (16u << 20)) return 0;
    int entsize = (mem_eq(sdt, "XSDT", 4)) ? 8 : 4;
    for (uint32_t off = 36; off + (uint32_t)entsize <= len; off += (uint32_t)entsize) {
        uint64_t addr = (entsize == 8) ? rd64(sdt + off) : (uint64_t)rd32(sdt + off);
        if (!addr) continue;
        const uint8_t *t = acpi_ensure(addr, 0x1000);
        if (!t) continue;
        if (mem_eq(t, sig, 4)) {
            uint32_t tl = rd32(t + 4);
            if (tl >= 36 && tl <= (16u << 20)) t = acpi_ensure(addr, tl);
            return t;
        }
    }
    return 0;
}

/* AML PkgLength cozumu; *pp ilerletilir, govde basi doner. */
static const uint8_t *aml_pkglen(const uint8_t *p, uint32_t *len_out) {
    uint8_t b0 = *p;
    int n = (b0 >> 6) + 1;
    uint32_t len = b0 & 0x3Fu;
    if (n >= 2) len |= (uint32_t)p[1] << 4;
    if (n >= 3) len |= (uint32_t)p[2] << 12;
    if (n >= 4) len |= (uint32_t)p[3] << 20;
    *len_out = len;
    return p + n;
}

/* Tek bir AML veri ogesini (tamsayi) cozer; p ilerletilir. */
static uint64_t aml_int(const uint8_t **pp, const uint8_t *end) {
    const uint8_t *p = *pp;
    uint64_t v = 0;
    if (p >= end) return 0;
    uint8_t op = *p++;
    if (op == 0x00) { v = 0; }
    else if (op == 0x01) { v = 1; }
    else if (op == 0x0A && p < end) { v = *p++; }
    else if (op == 0x0B && p + 1 < end) { v = rd16(p); p += 2; }
    else if (op == 0x0C && p + 3 < end) { v = rd32(p); p += 4; }
    else if (op == 0x0E && p + 7 < end) { v = rd64(p); p += 8; }
    *pp = p;
    return v;
}

/* DSDT icinde "_S5_" / "_S3_" paketini bulup SLP_TYP degerlerini yazar. */
static int dsdt_slp(const uint8_t *dsdt, const char *name, uint8_t *a, uint8_t *b) {
    if (!dsdt) return 0;
    uint32_t len = rd32(dsdt + 4);
    if (len < 36 || len > (16u << 20)) return 0;
    const uint8_t *end = dsdt + len;
    for (const uint8_t *p = dsdt + 36; p + 6 < end; p++) {
        if (p[0] != (uint8_t)name[0] || p[1] != (uint8_t)name[1] ||
            p[2] != (uint8_t)name[2] || p[3] != (uint8_t)name[3]) continue;
        const uint8_t *q = p + 4;
        if (*q != 0x12) continue;          /* PackageOp bekleniyor */
        uint32_t plen;
        q = aml_pkglen(q + 1, &plen);
        if (q >= end) return 0;
        uint8_t nelem = *q++;              /* NumElements */
        if (nelem < 1) return 0;
        uint64_t v0 = aml_int(&q, end);
        uint64_t v1 = v0;
        if (nelem >= 2) v1 = aml_int(&q, end);
        *a = (uint8_t)(v0 & 0x07u);
        *b = (uint8_t)(v1 & 0x07u);
        return 1;
    }
    return 0;
}

static void parse_madt(const uint8_t *sdt) {
    const uint8_t *madt = sdt_find(sdt, "APIC");
    if (!madt) return;
    uint32_t len = rd32(madt + 4);
    if (len < 44 || len > (16u << 20) || !acpi_checksum(madt, len)) return;
    g_acpi.has_madt = 1;
    g_acpi.lapic_addr = (uint64_t)rd32(madt + 36);
    for (uint32_t off = 44; off + 2 <= len; ) {
        uint8_t type = madt[off];
        uint8_t elen = madt[off + 1];
        if (elen < 2 || off + elen > len) break;
        if (type == 0 && elen >= 8) {
            if (rd32(madt + off + 4) & 1u) g_acpi.cpu_count++;
        } else if (type == 9 && elen >= 12) {
            if (rd32(madt + off + 8) & 1u) g_acpi.cpu_count++;
        } else if (type == 1 && elen >= 12) {
            g_acpi.ioapic_count++;
        } else if (type == 5 && elen >= 12) {
            g_acpi.lapic_addr = rd64(madt + off + 4);
        }
        off += elen;
    }
}

static void parse_mcfg(const uint8_t *sdt) {
    const uint8_t *mcfg = sdt_find(sdt, "MCFG");
    if (!mcfg) return;
    uint32_t len = rd32(mcfg + 4);
    if (len < 60 || len > (16u << 20) || !acpi_checksum(mcfg, len)) return;
    g_acpi.has_mcfg = 1;
    g_acpi.mcfg_ecam_base = rd64(mcfg + 44);
    g_acpi.mcfg_start_bus = mcfg[54];
    g_acpi.mcfg_end_bus   = mcfg[55];
}

/* ------------------------------------------------------------------ */
/* CPU termal sensoru (DTS) + guvenli bosta bekleme                   */
/* ------------------------------------------------------------------ */

static inline void acpi_cpuid(uint32_t leaf, uint32_t *a, uint32_t *b,
                              uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(0u));
}

static inline uint64_t acpi_rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static void probe_cpu_thermal(void) {
    g_acpi.dts_supported = 0;
    g_acpi.tjmax_c = 100;

    uint32_t max_leaf = 0, ebx = 0, ecx = 0, edx = 0;
    acpi_cpuid(0u, &max_leaf, &ebx, &ecx, &edx);
    if (max_leaf < 6u) return;

    /* Intel GenuineIntel kontrolu: IA32_THERM_STATUS / IA32_TEMPERATURE_TARGET */
    int is_intel = (ebx == 0x756E6547u && edx == 0x49656E69u && ecx == 0x6C65746Eu);
    if (!is_intel) return;

    uint32_t a6 = 0, b6 = 0, c6 = 0, d6 = 0;
    acpi_cpuid(6u, &a6, &b6, &c6, &d6);
    if (!(a6 & 1u)) return; /* CPUID.06H:EAX[0] = Digital Thermal Sensor */

    g_acpi.dts_supported = 1;

    /* IA32_TEMPERATURE_TARGET (0x1A2) yalniz Nehalem+ (family 6, model >= 0x1A)
     * islemcilerde mevcuttur; aksi halde veya okuma aralik disiysa 100C yedegi. */
    uint32_t a1 = 0, b1 = 0, c1 = 0, d1 = 0;
    acpi_cpuid(1u, &a1, &b1, &c1, &d1);
    uint32_t fam   = (a1 >> 8) & 0xFu;
    uint32_t model = ((a1 >> 4) & 0xFu) | (((a1 >> 16) & 0xFu) << 4);
    if (fam == 0x6u && model >= 0x1Au) {
        uint64_t tgt = acpi_rdmsr(0x1A2u);
        int tj = (int)((tgt >> 16) & 0xFFu);
        if (tj >= 50 && tj <= 125) g_acpi.tjmax_c = tj;
    }
}

int acpi_cpu_temp_c(void) {
    if (!g_acpi.dts_supported) return -1;
    uint64_t st = acpi_rdmsr(0x19Cu); /* IA32_THERM_STATUS */
    if (!(st & (1ull << 31))) return -1; /* Reading Valid (bit 31) */
    int delta = (int)((st >> 16) & 0x7Fu);
    int tj = g_acpi.tjmax_c > 0 ? g_acpi.tjmax_c : 100;
    int temp = tj - delta;
    if (temp < 0) temp = 0;
    return temp;
}

void acpi_halt_idle(void) {
    uint64_t rflags;
    __asm__ volatile("pushfq; popq %0" : "=r"(rflags) :: "memory");
    if (rflags & 0x200ull) {
        __asm__ volatile("hlt" ::: "memory");
    } else {
        __asm__ volatile("pause" ::: "memory");
    }
}

/* ------------------------------------------------------------------ */
/* Baslatma                                                           */
/* ------------------------------------------------------------------ */

void acpi_init(void) {
    for (unsigned i = 0; i < sizeof(g_acpi); i++) ((uint8_t *)&g_acpi)[i] = 0;
    g_acpi.slp_typ_s5a = 5;   /* ACPI spec onerilen varsayilan */
    g_acpi.slp_typ_s5b = 5;
    g_acpi.tjmax_c = 100;

    probe_cpu_thermal();

    const uint8_t *rsdp = find_rsdp();
    if (!rsdp) {
        serial_printf("[acpi] present=0 dts=%d tjmax=%d\n",
                      g_acpi.dts_supported, g_acpi.tjmax_c);
        return;
    }

    uint8_t revision = rsdp[15];
    const uint8_t *sdt = 0;
    if (revision >= 2) {
        uint32_t rlen = rd32(rsdp + 20);
        if (rlen >= 36 && rlen <= 256 && acpi_checksum(rsdp, rlen)) {
            uint64_t xsdt = rd64(rsdp + 24);
            if (xsdt) {
                const uint8_t *t = acpi_ensure(xsdt, 0x1000);
                if (t && mem_eq(t, "XSDT", 4)) {
                    uint32_t xlen = rd32(t + 4);
                    if (xlen >= 36 && xlen <= (16u << 20)) {
                        t = acpi_ensure(xsdt, xlen);
                        if (t && acpi_checksum(t, xlen)) {
                            sdt = t;
                            g_acpi.has_xsdt = 1;
                            g_acpi.sdt_addr = xsdt;
                        }
                    }
                }
            }
        }
    }
    if (!sdt) {
        uint32_t rsdt = rd32(rsdp + 16);
        if (rsdt) {
            const uint8_t *t = acpi_ensure((uint64_t)rsdt, 0x1000);
            if (t && mem_eq(t, "RSDT", 4)) {
                uint32_t rlen = rd32(t + 4);
                if (rlen >= 36 && rlen <= (16u << 20)) {
                    t = acpi_ensure((uint64_t)rsdt, rlen);
                    if (t && acpi_checksum(t, rlen)) {
                        sdt = t;
                        g_acpi.sdt_addr = (uint64_t)rsdt;
                    }
                }
            }
        }
    }
    if (!sdt) {
        serial_printf("[acpi] present=0 (no valid SDT) dts=%d\n", g_acpi.dts_supported);
        return;
    }

    parse_madt(sdt);
    parse_mcfg(sdt);

    const uint8_t *fadt = sdt_find(sdt, "FACP");
    if (fadt) {
        uint32_t flen = rd32(fadt + 4);
        if (flen >= 0x40 && flen <= (16u << 20)) {
            for (int i = 0; i < 6; i++) g_acpi.oem_id[i] = (char)fadt[0x0A + i];
            g_acpi.oem_id[6] = 0;
            g_acpi.present = 1;
            if (flen >= 0x2Eu) g_acpi.pm_profile = fadt[0x2D];
            if (flen >= 0x30u) g_acpi.sci_int = (uint8_t)rd16(fadt + 0x2E);
            if (flen >= 0x36u) {
                g_acpi.smi_cmd = (uint16_t)rd32(fadt + 0x30);
                g_acpi.acpi_enable = fadt[0x34];
                g_acpi.acpi_disable = fadt[0x35];
            }
            if (flen >= 0x3Cu) g_acpi.pm1a_evt = (uint16_t)rd32(fadt + 0x38);
            if (flen >= 0x44u) g_acpi.pm1a_cnt = (uint16_t)rd32(fadt + 0x40);
            if (flen >= 0x48u) g_acpi.pm1b_cnt = (uint16_t)rd32(fadt + 0x44);
            if (flen >= 0x5Au) g_acpi.pm1_cnt_len = fadt[0x59];
            if (flen >= 0x6Du) g_acpi.century = fadt[0x6C];
            if (flen >= 0x74u) g_acpi.flags = rd32(fadt + 0x70);

            /* DSDT: ACPI 2.0+ X_DSDT (offset 0x8C, 64-bit) tercih edilir. */
            uint64_t dsdt_addr = (flen >= 0x2Cu) ? (uint64_t)rd32(fadt + 0x28) : 0ull;
            if (flen >= 0x94u) {
                uint64_t xd = rd64(fadt + 0x8C);
                if (xd) dsdt_addr = xd;
            }
            const uint8_t *dsdt = dsdt_addr ? acpi_ensure(dsdt_addr, 0x1000) : 0;
            if (dsdt && mem_eq(dsdt, "DSDT", 4)) {
                uint32_t dlen = rd32(dsdt + 4);
                if (dlen >= 36 && dlen <= (16u << 20)) {
                    dsdt = acpi_ensure(dsdt_addr, dlen);
                    if (dsdt) {
                        g_acpi.has_dsdt = 1;
                        dsdt_slp(dsdt, "_S5_", &g_acpi.slp_typ_s5a, &g_acpi.slp_typ_s5b);
                        g_acpi.has_s3 = dsdt_slp(dsdt, "_S3_", &g_acpi.slp_typ_s3a, &g_acpi.slp_typ_s3b);
                    }
                }
            }
        }
    }

    int temp_c = acpi_cpu_temp_c();
    serial_printf("[acpi] oem=%s %s=0x%llX fadt=%d dsdt=%d s3=%d cpus=%u mcfg=0x%llX prof=%u dts=%d temp=%dC\n",
                  g_acpi.oem_id[0] ? g_acpi.oem_id : "-",
                  g_acpi.has_xsdt ? "xsdt" : "rsdt",
                  (unsigned long long)g_acpi.sdt_addr,
                  g_acpi.present, g_acpi.has_dsdt, g_acpi.has_s3,
                  g_acpi.cpu_count,
                  (unsigned long long)g_acpi.mcfg_ecam_base,
                  (unsigned)g_acpi.pm_profile,
                  g_acpi.dts_supported, temp_c);
}

int acpi_present(void) { return g_acpi.present; }

/* SCI_EN biti (PM1_CNT bit0). Kapaliysa SMI uzerinden ACPI'yi acar. */
static void acpi_enable_sci(void) {
    if (!g_acpi.present || !g_acpi.pm1a_cnt) return;
    if (inw(g_acpi.pm1a_cnt) & 0x0001) return;
    if (g_acpi.smi_cmd && g_acpi.acpi_enable) {
        outb(g_acpi.smi_cmd, g_acpi.acpi_enable);
        for (int i = 0; i < 100000; i++) {
            if (inw(g_acpi.pm1a_cnt) & 0x0001) return;
            io_wait();
        }
    }
}

void acpi_shutdown(void) {
    if (g_acpi.present && g_acpi.pm1a_cnt) {
        acpi_enable_sci();
        uint16_t va = (uint16_t)(((uint16_t)g_acpi.slp_typ_s5a & 0x07u) << 10) | 0x2000u;
        outw(g_acpi.pm1a_cnt, va);
        if (g_acpi.pm1b_cnt) {
            uint16_t vb = (uint16_t)(((uint16_t)g_acpi.slp_typ_s5b & 0x07u) << 10) | 0x2000u;
            outw(g_acpi.pm1b_cnt, vb);
        }
        /* Guc kesilmediyse kisa sure bekle, sonra eski yonteme dus. */
        for (int i = 0; i < 200000; i++) io_wait();
    }
    /* QEMU / Bochs yedek portlari */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    for (;;) { asm volatile("cli"); asm volatile("hlt"); }
}

int acpi_enter_sleep(void) {
    if (!g_acpi.present || !g_acpi.pm1a_cnt || !g_acpi.has_s3) return 0;
    if (g_acpi.slp_typ_s3a == g_acpi.slp_typ_s5a) return 0;
    acpi_enable_sci();
    uint16_t va = (uint16_t)(((uint16_t)g_acpi.slp_typ_s3a & 0x07u) << 10) | 0x2000u;
    outw(g_acpi.pm1a_cnt, va);
    if (g_acpi.pm1b_cnt) {
        uint16_t vb = (uint16_t)(((uint16_t)g_acpi.slp_typ_s3b & 0x07u) << 10) | 0x2000u;
        outw(g_acpi.pm1b_cnt, vb);
    }
    for (int i = 0; i < 100000; i++) io_wait();
    return 1;
}

int acpi_sleep_supported(void) {
    if (!g_acpi.present || !g_acpi.pm1a_cnt || !g_acpi.has_s3) return 0;
    return g_acpi.slp_typ_s3a != g_acpi.slp_typ_s5a;
}

/* ------------------------------------------------------------------ */
/* SMBus (PIIX4/ICH) + Smart Battery (SBS) - en iyi caba             */
/* ------------------------------------------------------------------ */

#define SBS_ADDR        0x0B   /* Smart Battery 7-bit adresi */
#define SBS_REL_SOC     0x0D   /* RelativeStateOfCharge */
#define SBS_BAT_STATUS  0x16
#define SBS_AVG_TTE     0x13   /* AverageTimeToEmpty */

static uint16_t s_smb_base = 0;
static int      s_smb_checked = 0;
static int      s_bat_present = 0;
static int      s_bat_percent = -1;
static int      s_bat_charging = 0;
static int      s_bat_minutes = -1;

static void smbus_probe(void) {
    s_smb_checked = 1;
    for (int bus = 0; bus < 8 && !s_smb_base; bus++) {
        for (int slot = 0; slot < 32 && !s_smb_base; slot++) {
            for (int f = 0; f < 8; f++) {
                uint16_t ven = pci_config_readw((uint8_t)bus, (uint8_t)slot, (uint8_t)f, 0x00);
                if (ven == 0xFFFF) break;
                uint8_t cls = pci_config_readb((uint8_t)bus, (uint8_t)slot, (uint8_t)f, 0x0B);
                uint8_t sub = pci_config_readb((uint8_t)bus, (uint8_t)slot, (uint8_t)f, 0x0A);
                if (cls == 0x0C && sub == 0x05) {
                    uint16_t ba = (uint16_t)(pci_config_readw((uint8_t)bus, (uint8_t)slot, (uint8_t)f, 0x90) & 0xFFF0u);
                    if (ba) { s_smb_base = ba; return; }
                }
            }
        }
    }
}

static int smbus_read_word(uint8_t addr, uint8_t cmd, uint16_t *out) {
    uint16_t b = s_smb_base;
    if (!b) return -1;
    for (int i = 0; i < 10000; i++) { if (!(inb((uint16_t)(b + 0)) & 0x01)) break; io_wait(); }
    outb((uint16_t)(b + 4), (uint8_t)(addr << 1));   /* SMBHSTADD */
    outb((uint16_t)(b + 3), cmd);                    /* SMBHSTCMD */
    outb((uint16_t)(b + 2), 0x4C);                   /* START | WORD_DATA */
    uint8_t st = 0;
    for (int i = 0; i < 300000; i++) {
        st = inb((uint16_t)(b + 0));
        if (st & 0x02) break;              /* INTR */
        if (st & 0x04) { outb((uint16_t)(b + 0), 0x1E); return -1; } /* BUS_ERR */
        io_wait();
    }
    if (!(st & 0x02)) { outb((uint16_t)(b + 0), 0x1E); return -1; }
    uint16_t lo = inb((uint16_t)(b + 5));
    uint16_t hi = inb((uint16_t)(b + 6));
    *out = (uint16_t)(lo | (hi << 8));
    outb((uint16_t)(b + 0), 0x1E);        /* durum bitlerini temizle */
    return 0;
}

static void smbus_sbs_poll(void) {
    if (!s_smb_checked) smbus_probe();
    if (!s_smb_base) return;
    uint16_t soc;
    if (smbus_read_word(SBS_ADDR, SBS_REL_SOC, &soc) == 0 && soc <= 100) {
        s_bat_present = 1;
        s_bat_percent = (int)soc;
        uint16_t st;
        if (smbus_read_word(SBS_ADDR, SBS_BAT_STATUS, &st) == 0)
            s_bat_charging = (st & 0x0001) ? 0 : 1;   /* bit0 = Discharging */
        uint16_t tte;
        if (smbus_read_word(SBS_ADDR, SBS_AVG_TTE, &tte) == 0 && tte != 0xFFFF)
            s_bat_minutes = (int)tte;
    } else {
        s_bat_present = 0;
        s_bat_percent = -1;
    }
}

void acpi_power_poll(acpi_power_t *out) {
    if (!out) return;
    out->source = 0;
    out->present = 0;
    out->percent = -1;
    out->charging = 0;
    out->minutes = -1;
    out->smbus = s_smb_base ? 1 : 0;

    smbus_sbs_poll();
    if (s_bat_present) {
        out->present = 1;
        out->percent = s_bat_percent;
        out->charging = s_bat_charging;
        out->minutes = s_bat_minutes;
        out->source = s_bat_charging ? 0 : 1;   /* sarjda AC, degilse pil */
        out->smbus = 1;
    }
}

int acpi_selftest(void) {
    serial_printf("[acpi] present=%d oem=%s pm1a_cnt=0x%x smi=0x%x "
                  "S5=%u/%u S3=%u/%u s3_var=%d dsdt=%d mcfg=0x%llX temp=%dC\n",
                  g_acpi.present, g_acpi.oem_id[0] ? g_acpi.oem_id : "-",
                  g_acpi.pm1a_cnt, g_acpi.smi_cmd,
                  g_acpi.slp_typ_s5a, g_acpi.slp_typ_s5b,
                  g_acpi.slp_typ_s3a, g_acpi.slp_typ_s3b, g_acpi.has_s3,
                  g_acpi.has_dsdt, (unsigned long long)g_acpi.mcfg_ecam_base,
                  acpi_cpu_temp_c());
    smbus_probe();
    acpi_power_t p;
    acpi_power_poll(&p);
    serial_printf("[acpi] smbus=0x%x bat=%d pct=%d chg=%d src=%d\n",
                  s_smb_base, p.present, p.percent, p.charging, p.source);
    return g_acpi.present ? 0 : 1;
}
