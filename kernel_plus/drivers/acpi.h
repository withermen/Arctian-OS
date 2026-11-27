#ifndef ARCTIAN_ACPI_H
#define ARCTIAN_ACPI_H

#include <stdint.h>
#include <stdbool.h>

/* ACPI tablo kesfi (RSDP -> RSDT/XSDT -> FADT/DSDT/MADT/MCFG) ve guc yonetimi.
 * Gercek S5 kapatma FADT'nin PM1a/PM1b kontrol blogu ve DSDT'teki _S5
 * paketindeki SLP_TYP degerleriyle yapilir. Pil/AC durumu SMBus (SBS)
 * uzerinden en-iyi-caba okunur; yoksa AC varsayilir. */

typedef struct {
    int      present;       /* FADT bulundu mu */
    int      has_dsdt;
    int      has_xsdt;      /* XSDT (64-bit) kullanildi mi */
    int      has_madt;
    int      has_mcfg;
    uint16_t pm1a_cnt;      /* PM1a kontrol blogu G/IO portu */
    uint16_t pm1b_cnt;
    uint16_t pm1a_evt;
    uint16_t smi_cmd;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  pm1_cnt_len;
    uint8_t  sci_int;
    uint8_t  slp_typ_s5a;
    uint8_t  slp_typ_s5b;
    uint8_t  slp_typ_s3a;
    uint8_t  slp_typ_s3b;
    int      has_s3;        /* DSDT'te _S3_ paketi bulundu mu (0 = uyku yok) */
    uint8_t  century;
    uint8_t  pm_profile;    /* FADT Preferred_PM_Profile (0..8) */
    uint8_t  mcfg_start_bus;
    uint8_t  mcfg_end_bus;
    uint32_t flags;
    uint32_t cpu_count;     /* MADT etkin LAPIC/x2APIC sayisi */
    uint32_t ioapic_count;
    uint64_t lapic_addr;
    uint64_t sdt_addr;      /* Kullanilan XSDT veya RSDT fiziksel adresi */
    uint64_t mcfg_ecam_base;/* MCFG PCIe ECAM taban adresi (yoksa 0) */
    int      dts_supported; /* CPUID.06H:EAX[0] dijital termal sensor */
    int      tjmax_c;       /* TjMax (C), varsayilan 100 */
    uint64_t facs_addr;     /* FACS tablosu fiziksel adresi (yoksa 0) */
    uint32_t s3_waking_vector; /* FACS 16-bit S3 waking vector (0 = hazir degil) */
    char     oem_id[7];
} acpi_info_t;

extern acpi_info_t g_acpi;

void acpi_init(void);
int  acpi_present(void);
void acpi_shutdown(void);        /* S5: gercekten gucu keser (ACPI varsa) */
int  acpi_enter_sleep(void);     /* S3 dener; waking vector yoksa -1 doner */
int  acpi_sleep_supported(void); /* 1 = bu makine S3'e uyutabilir */

/* CPU termal sensoru (C) ve guvenli bosta bekleme */
int  acpi_cpu_temp_c(void);      /* -1 = DTS yok veya gecersiz okuma */
void acpi_halt_idle(void);       /* IF=1 ise hlt, degilse pause */

/* Guc / pil durumu */
typedef struct {
    int source;     /* 0 = AC, 1 = pil */
    int present;    /* pil takili mi */
    int percent;    /* 0..100, -1 = bilinmiyor */
    int charging;   /* 1 = sarj oluyor */
    int minutes;    /* kalan sure (dk), -1 = bilinmiyor */
    int smbus;      /* SMBus denetleyicisi bulundu mu */
} acpi_power_t;

void acpi_power_poll(acpi_power_t *out);

int  acpi_selftest(void);

#endif /* ARCTIAN_ACPI_H */
