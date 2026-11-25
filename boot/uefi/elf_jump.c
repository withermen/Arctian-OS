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
static UINT64 s_extra_pts[16][512] __attribute__((aligned(4096)));

static void u_memmove(void *dst, const void *src, UINT64 n) {
    UINT8 *d = (UINT8 *)dst;
    const UINT8 *s = (const UINT8 *)src;
    if (d == s || n == 0) return;
    if ((uintptr_t)d < (uintptr_t)s || (uintptr_t)d >= (uintptr_t)s + n) {
        for (UINT64 i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (UINT64 i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
}

static void u_memcpy(void *dst, const void *src, UINT64 n) {
    u_memmove(dst, src, n);
}

static void u_memset(void *dst, UINT8 v, UINT64 n) {
    UINT8 *d = (UINT8 *)dst;
    for (UINT64 i = 0; i < n; i++) d[i] = v;
}

typedef struct {
    UINT64 low_next;
    UINTN  extra_used;
} pt_alloc_t;

static volatile UINT64 *alloc_pt_page(pt_alloc_t *al) {
    volatile UINT64 *p = 0;
    if (al->low_next + 0x1000ull <= 0x20000ull) {
        p = (volatile UINT64 *)(uintptr_t)al->low_next;
        al->low_next += 0x1000ull;
    } else if (al->extra_used < (sizeof(s_extra_pts) / sizeof(s_extra_pts[0]))) {
        p = (volatile UINT64 *)s_extra_pts[al->extra_used++];
    } else {
        return 0;
    }
    for (int i = 0; i < 512; i++) p[i] = 0;
    return p;
}

static void map_range_2m(pt_alloc_t *al, volatile UINT64 *pml4,
                         UINT64 va_base, UINT64 pa_base, UINT64 size,
                         UINT64 leaf_flags) {
    if (size == 0) return;
    UINT64 off = va_base & 0x1FFFFFull;
    UINT64 va_start = va_base & ~0x1FFFFFull;
    UINT64 pa_start = pa_base & ~0x1FFFFFull;
    UINT64 span = off + size;
    UINT64 va_end = (va_start + span + 0x1FFFFFull) & ~0x1FFFFFull;
    if (va_end <= va_start) return;

    UINT64 pages = (va_end - va_start) >> 21;
    for (UINT64 k = 0; k < pages; k++) {
        UINT64 va = va_start + (k << 21);
        UINT64 pa = pa_start + (k << 21);
        UINTN i4 = (UINTN)((va >> 39) & 0x1FFu);
        UINTN i3 = (UINTN)((va >> 30) & 0x1FFu);
        UINTN i2 = (UINTN)((va >> 21) & 0x1FFu);

        volatile UINT64 *pdp;
        if (pml4[i4] & 1ull) {
            pdp = (volatile UINT64 *)(uintptr_t)(pml4[i4] & ~0xFFFull);
        } else {
            pdp = alloc_pt_page(al);
            if (!pdp) return;
            pml4[i4] = (UINT64)(uintptr_t)pdp | 0x03ull;
        }

        volatile UINT64 *pd;
        if (pdp[i3] & 1ull) {
            if (pdp[i3] & 0x80ull) continue;
            pd = (volatile UINT64 *)(uintptr_t)(pdp[i3] & ~0xFFFull);
        } else {
            pd = alloc_pt_page(al);
            if (!pd) return;
            pdp[i3] = (UINT64)(uintptr_t)pd | 0x03ull;
        }

        pd[i2] = pa | leaf_flags;
    }
}

static void build_identity_pagetables(const elf64_ehdr_t *eh, const UINT8 *img,
                                      UINT64 fb_phys, UINT64 fb_size) {
    /* stage2.asm ve vesa.c ile birebir ayni taban adresler:
     *   0x11000 = PML4, 0x12000 = PDP0, 0x13000 = PD0 (512 x 2 MB = 1 GB). */
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

    pt_alloc_t al;
    al.low_next = 0x14000ull;
    al.extra_used = 0;

    /* 1 GB uzerindeki PT_LOAD segmentlerini ve gerekirse sanal->fiziksel eslemeyi kur. */
    for (UINT16 i = 0; i < eh->e_phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)(img + eh->e_phoff +
                                                        (UINT64)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
        UINT64 pa = ph->p_paddr ? ph->p_paddr : ph->p_vaddr;
        if (pa + ph->p_memsz > 0x40000000ull)
            map_range_2m(&al, pml4, pa, pa, ph->p_memsz, 0x83ull);
        if (ph->p_vaddr && ph->p_vaddr != pa &&
            (ph->p_vaddr & 0x1FFFFFull) == (pa & 0x1FFFFFull)) {
            map_range_2m(&al, pml4, ph->p_vaddr, pa, ph->p_memsz, 0x83ull);
        }
    }

    /* 64-bit GOP framebuffer (1 GB / 4 GB ustu dahil) WC/PWT 2MB sayfa eslemesi. */
    if (fb_phys != 0) {
        UINT64 fsz = fb_size ? fb_size : (16ull * 1024ull * 1024ull);
        map_range_2m(&al, pml4, fb_phys, fb_phys, fsz, 0x8Bull); /* P|RW|PWT|PS */
    }
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
        eh->e_ident[4] != 2    || eh->e_ident[5] != 1   ||
        eh->e_type != 2        || eh->e_machine != 0x3Eu) {
        serial_puts("ARCTIAN-UEFI-ELF: gecersiz ELF64 basligi\r\n");
        return;
    }
    if (eh->e_phnum == 0 || eh->e_phentsize < sizeof(elf64_phdr_t) ||
        eh->e_phoff > img_sz ||
        (UINT64)eh->e_phnum * eh->e_phentsize > img_sz - eh->e_phoff) {
        serial_puts("ARCTIAN-UEFI-ELF: bozuk program tablosu\r\n");
        return;
    }

    int load_cnt = 0;
    int entry_ok = 0;
    for (UINT16 i = 0; i < eh->e_phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)(img + eh->e_phoff +
                                                        (UINT64)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
        if (ph->p_filesz > ph->p_memsz || ph->p_offset > img_sz ||
            ph->p_filesz > img_sz - ph->p_offset) {
            serial_puts("ARCTIAN-UEFI-ELF: segment sinir disi\r\n");
            return;
        }
        UINT64 pa = ph->p_paddr ? ph->p_paddr : ph->p_vaddr;
        if (pa < 0x20000ull || pa + ph->p_memsz < pa) {
            serial_puts("ARCTIAN-UEFI-ELF: hedef adres aralik disi pa=");
            serial_hex(pa);
            serial_puts("\r\n");
            return;
        }
        if ((eh->e_entry >= pa && eh->e_entry < pa + ph->p_memsz) ||
            (ph->p_vaddr && eh->e_entry >= ph->p_vaddr &&
             eh->e_entry < ph->p_vaddr + ph->p_memsz)) {
            entry_ok = 1;
        }
        load_cnt++;
    }
    if (load_cnt == 0 || !entry_ok) {
        serial_puts("ARCTIAN-UEFI-ELF: gecerli PT_LOAD/entry bulunamadi\r\n");
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

    /* stage2 uyumlu sayfa tablosunu + 64-bit GOP/PT_LOAD haritalarini kur ve
     * PIC kopruyu 0x9000'e kopyala. */
    UINT64 fb_phys = ui->fb_phys;
    UINT64 fb_size = ui->fb_size;
    if (fb_phys == 0) {
        const arctian_bootinfo_t *bi = arctian_bootinfo();
        if (arctian_bootinfo_valid(bi) && bi->fb_phys != 0xFFFFFFFFu) {
            fb_phys = (UINT64)bi->fb_phys;
            fb_size = (UINT64)bi->fb_size;
        }
    }
    if (fb_phys >= 0x40000000ull) {
        serial_puts("ARCTIAN-UEFI-JUMP: fb_map=");
        serial_hex(fb_phys);
        serial_puts("\r\n");
    }
    build_identity_pagetables(eh, img, fb_phys, fb_size);

    UINT64 stub_len = (UINT64)(uefi_handoff_stub_end - uefi_handoff_stub);
    u_memcpy((void *)(uintptr_t)0x9000u, uefi_handoff_stub, stub_len);

    typedef void (EFIAPI *handoff_fn_t)(UINT64 entry_addr, UINT64 pml4_addr);
    handoff_fn_t go = (handoff_fn_t)(uintptr_t)0x9000u;
    go(entry, 0x11000ull);
}
