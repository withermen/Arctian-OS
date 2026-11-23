/*
 * Arctian UEFI onyukleyici - paket 3: ELF64 yukleme + ExitBootServices +
 * stage2 uyumlu sayfa tablosu + 64-bit cekirdek atlamasi.
 *
 * Akis:
 *   1. fs_load_kernel()'in EfiLoaderData havuzuna okudugu KERNEL.ELF'i dogrula.
 *   2. Watchdog'u kapat, GetMemoryMap + ExitBootServices (yeniden denemeli).
 *   3. PT_LOAD bolumlerini fiziksel adreslerine (0x100000+) kopyala, BSS sifirla.
 *   4. stage2.asm ile ayni adreste (0x11000/0x12000/0x13000) 1 GB ozdeslik
 *      haritasi (512 x 2 MB) kur.
 *   5. PIC guvenli uefi_handoff_stub'i 0x9000'e kopyala ve e_entry'ye atla.
 */
#include "loader.h"
#include "bootinfo.h"
#include "uefi_info.h"

typedef struct {
    UINT8  e_ident[16];
    UINT16 e_type;
    UINT16 e_machine;
    UINT32 e_version;
    UINT64 e_entry;
    UINT64 e_phoff;
    UINT64 e_shoff;
    UINT32 e_flags;
    UINT16 e_ehsize;
    UINT16 e_phentsize;
    UINT16 e_phnum;
    UINT16 e_shentsize;
    UINT16 e_shnum;
    UINT16 e_shstrndx;
} __attribute__((packed)) elf64_ehdr_t;

typedef struct {
    UINT32 p_type;
    UINT32 p_flags;
    UINT64 p_offset;
    UINT64 p_vaddr;
    UINT64 p_paddr;
    UINT64 p_filesz;
    UINT64 p_memsz;
    UINT64 p_align;
} __attribute__((packed)) elf64_phdr_t;

#define PT_LOAD 1u

extern const UINT8 uefi_handoff_stub[];
extern const UINT8 uefi_handoff_stub_end[];

static UINT8 s_mmap_buf[32768] __attribute__((aligned(16)));

static void u_memcpy(void *dst, const void *src, UINT64 n) {
    UINT8 *d = (UINT8 *)dst;
    const UINT8 *s = (const UINT8 *)src;
    for (UINT64 i = 0; i < n; i++) d[i] = s[i];
}

static void u_memset(void *dst, UINT8 v, UINT64 n) {
    UINT8 *d = (UINT8 *)dst;
    for (UINT64 i = 0; i < n; i++) d[i] = v;
}

static void build_identity_pagetables(void) {
    /* stage2.asm ve vesa.c ile birebir ayni fiziksel adresler:
     *   0x11000 = PML4, 0x12000 = PDP, 0x13000 = PD (512 x 2 MB = 1 GB). */
    volatile UINT64 *pml4 = (volatile UINT64 *)(uintptr_t)0x11000u;
    volatile UINT64 *pdp  = (volatile UINT64 *)(uintptr_t)0x12000u;
    volatile UINT64 *pd   = (volatile UINT64 *)(uintptr_t)0x13000u;

    for (int i = 0; i < 512; i++) {
        pml4[i] = 0;
        pdp[i]  = 0;
        pd[i]   = ((UINT64)i * 0x200000ull) | 0x83ull;  /* P | RW | PS(2MB) */
    }
    pml4[0] = 0x12000ull | 0x03ull;
    pdp[0]  = 0x13000ull | 0x03ull;
}

void uefi_boot_kernel(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *st) {
    arctian_uefiinfo_t *ui = arctian_uefiinfo();
    if (!ui->kernel_base || ui->kernel_size < sizeof(elf64_ehdr_t))
        return;  /* KERNEL.ELF yuklenmediyse firmware'e temiz don */

    const UINT8 *img = (const UINT8 *)(uintptr_t)ui->kernel_base;
    UINT64 img_sz = ui->kernel_size;
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)img;

    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F' ||
        eh->e_ident[4] != 2    || eh->e_machine != 0x3Eu) {
        serial_puts("ARCTIAN-UEFI-ELF: gecersiz ELF64 basligi\r\n");
        return;
    }
    if (eh->e_phoff + (UINT64)eh->e_phnum * eh->e_phentsize > img_sz ||
        eh->e_phentsize < sizeof(elf64_phdr_t)) {
        serial_puts("ARCTIAN-UEFI-ELF: bozuk program tablosu\r\n");
        return;
    }

    int load_cnt = 0;
    for (UINT16 i = 0; i < eh->e_phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)(img + eh->e_phoff +
                                                        (UINT64)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
        if (ph->p_filesz > ph->p_memsz || ph->p_offset + ph->p_filesz > img_sz) {
            serial_puts("ARCTIAN-UEFI-ELF: segment sinir disi\r\n");
            return;
        }
        UINT64 pa = ph->p_paddr ? ph->p_paddr : ph->p_vaddr;
        if (pa < 0x100000ull || pa + ph->p_memsz > 0x40000000ull) {
            serial_puts("ARCTIAN-UEFI-ELF: hedef adres aralik disi\r\n");
            return;
        }
        load_cnt++;
    }
    if (load_cnt == 0 || eh->e_entry < 0x100000ull) {
        serial_puts("ARCTIAN-UEFI-ELF: PT_LOAD bulunamadi\r\n");
        return;
    }

    serial_puts("ARCTIAN-UEFI-ELF: entry=");
    serial_hex(eh->e_entry);
    serial_puts(" ph=");
    serial_dec((UINT64)load_cnt);
    serial_puts("\r\n");

    EFI_BOOT_SERVICES *bs = st ? st->BootServices : 0;
    if (!bs || !bs->GetMemoryMap || !bs->ExitBootServices) {
        serial_puts("ARCTIAN-UEFI-JUMP: BootServices eksik\r\n");
        return;
    }

    if (bs->SetWatchdogTimer)
        bs->SetWatchdogTimer(0, 0, 0, 0);

    /* GetMemoryMap + ExitBootServices: UEFI sartnamesine gore MapKey
     * degisirse birkac kez tekrar dene. */
    EFI_STATUS ebs_st = 1;
    for (int tries = 0; tries < 5; tries++) {
        UINTN mmap_sz = sizeof(s_mmap_buf);
        UINTN map_key = 0;
        UINTN desc_sz = 0;
        UINT32 desc_ver = 0;
        EFI_STATUS ms = bs->GetMemoryMap(&mmap_sz, s_mmap_buf, &map_key,
                                         &desc_sz, &desc_ver);
        if (ms != EFI_SUCCESS) {
            serial_puts("ARCTIAN-UEFI-JUMP: GetMemoryMap st=");
            serial_hex(ms);
            serial_puts("\r\n");
            return;
        }
        ui->memmap_addr = (uint64_t)(uintptr_t)s_mmap_buf;
        ui->memmap_bytes = (uint64_t)mmap_sz;
        ui->memmap_desc_size = (uint32_t)desc_sz;
        ui->memmap_count = desc_sz ? (uint32_t)(mmap_sz / desc_sz) : 0;
        ui->memmap_key = (uint64_t)map_key;

        ebs_st = bs->ExitBootServices(ImageHandle, map_key);
        if (ebs_st == EFI_SUCCESS) break;
    }
    if (ebs_st != EFI_SUCCESS) {
        serial_puts("ARCTIAN-UEFI-JUMP: ExitBootServices st=");
        serial_hex(ebs_st);
        serial_puts("\r\n");
        return;
    }

    /* Artık makinenin tam sahibiyiz (BootServices cagrilamaz; COM1 serbest). */
    __asm__ __volatile__("cli");
    serial_puts("ARCTIAN-UEFI-JUMP: ebs=OK entry=");
    serial_hex(eh->e_entry);
    serial_puts("\r\n");

    UINT64 entry = eh->e_entry;

    /* ELF PT_LOAD bolumlerini hedef fiziksel adreslerine yerlestir. */
    for (UINT16 i = 0; i < eh->e_phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)(img + eh->e_phoff +
                                                        (UINT64)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
        UINT64 pa = ph->p_paddr ? ph->p_paddr : ph->p_vaddr;
        if (ph->p_filesz > 0)
            u_memcpy((void *)(uintptr_t)pa, img + ph->p_offset, ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz)
            u_memset((void *)(uintptr_t)(pa + ph->p_filesz), 0,
                     ph->p_memsz - ph->p_filesz);
    }

    /* stage2 uyumlu sayfa tablosunu kur ve PIC kopruyu 0x9000'e kopyala. */
    build_identity_pagetables();

    UINT64 stub_len = (UINT64)(uefi_handoff_stub_end - uefi_handoff_stub);
    u_memcpy((void *)(uintptr_t)0x9000u, uefi_handoff_stub, stub_len);

    typedef void (EFIAPI *handoff_fn_t)(UINT64 entry_addr, UINT64 pml4_addr);
    handoff_fn_t go = (handoff_fn_t)(uintptr_t)0x9000u;
    go(entry, 0x11000ull);
}
