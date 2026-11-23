/*
 * Arctian UEFI onyukleyici - ACPI RSDP bulucu (paket 2/3).
 *
 * SystemTable->ConfigurationTable icinde ACPI 2.0 (tercihli) veya ACPI 1.0
 * RSDP tablosunu bulur, 20 bayt saglama toplamini dogrular ve 0x7100
 * (arctian_uefiinfo_t) yapisina yazar.
 */
#include "loader.h"
#include "uefi_info.h"

/* EFI_ACPI_20_TABLE_GUID {8868e871-e4f1-11d3-bc22-0080c73c8881} */
static const EFI_GUID g_acpi20_guid = {
    0x8868E871u, 0xE4F1u, 0x11D3u,
    { 0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81 }
};

/* ACPI_TABLE_GUID (1.0) {eb9d2d30-2d88-11d3-9a16-0090273fc14d} */
static const EFI_GUID g_acpi10_guid = {
    0xEB9D2D30u, 0x2D88u, 0x11D3u,
    { 0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D }
};

static int guid_eq(const EFI_GUID *a, const EFI_GUID *b) {
    if (a->Data1 != b->Data1 || a->Data2 != b->Data2 || a->Data3 != b->Data3)
        return 0;
    for (int i = 0; i < 8; i++)
        if (a->Data4[i] != b->Data4[i]) return 0;
    return 1;
}

static int rsdp_sum20(const UINT8 *p) {
    UINT8 s = 0;
    for (int i = 0; i < 20; i++) s = (UINT8)(s + p[i]);
    return s == 0;
}

void rsdp_find(EFI_SYSTEM_TABLE *st) {
    if (!st || !st->ConfigurationTable || !st->NumberOfTableEntries) return;
    const EFI_CONFIGURATION_TABLE *ct =
        (const EFI_CONFIGURATION_TABLE *)st->ConfigurationTable;
    const UINT8 *found = 0;
    int rev2 = 0;

    for (UINTN i = 0; i < st->NumberOfTableEntries; i++) {
        if (guid_eq(&ct[i].VendorGuid, &g_acpi20_guid) && ct[i].VendorTable) {
            found = (const UINT8 *)ct[i].VendorTable;
            rev2 = 1;
            break;
        }
        if (!found && guid_eq(&ct[i].VendorGuid, &g_acpi10_guid) && ct[i].VendorTable) {
            found = (const UINT8 *)ct[i].VendorTable;
            rev2 = 0;
        }
    }

    if (!found) {
        serial_puts("ARCTIAN-UEFI-ACPI: rsdp yok\r\n");
        return;
    }

    int sig_ok = (found[0] == 'R' && found[1] == 'S' && found[2] == 'D' &&
                  found[3] == ' ' && found[4] == 'P' && found[5] == 'T' &&
                  found[6] == 'R' && found[7] == ' ');
    int sum_ok = sig_ok && rsdp_sum20(found);

    arctian_uefiinfo_t *ui = arctian_uefiinfo();
    ui->rsdp_addr = (uint64_t)(uintptr_t)found;
    ui->rsdp_rev2 = (uint32_t)rev2;
    ui->rsdp_sum_ok = (uint32_t)sum_ok;

    serial_puts("ARCTIAN-UEFI-ACPI: rsdp=");
    serial_hex(ui->rsdp_addr);
    serial_puts(rev2 ? " rev2" : " rev1");
    serial_puts(sum_ok ? " sum=OK\r\n" : " sum=HATA\r\n");
}
