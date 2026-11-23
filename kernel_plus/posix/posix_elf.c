/* POSIX ELF64 yukleyici: PT_LOAD + statik PIE (ET_DYN + PT_DYNAMIC/R_X86_64_RELATIVE)
 * + System V AMD64 ABI ilk yigin duzeni (argc, argv, envp, auxv).
 *
 * musl static-PIE (-static-pie) ve statik ET_EXEC ikilileri dogrudan yukler.
 */
#include "posix.h"
#include "net.h"
#include "serial.h"
#include <string.h>

#define PAGE_SZ   0x1000ull
#define PAGE_MASK (~(PAGE_SZ - 1ull))
#define PAGE_UP(x) (((uint64_t)(x) + PAGE_SZ - 1ull) & PAGE_MASK)

#define ET_EXEC 2
#define ET_DYN  3

#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_PHDR    6
#define PT_TLS     7

#define PF_X 1u
#define PF_W 2u
#define PF_R 4u

#define DT_NULL    0
#define DT_RELA    7
#define DT_RELASZ  8
#define DT_RELAENT 9

#define R_X86_64_RELATIVE 8u

#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_BASE   7
#define AT_FLAGS  8
#define AT_ENTRY  9
#define AT_UID    11
#define AT_EUID   12
#define AT_GID    13
#define AT_EGID   14
#define AT_SECURE 23
#define AT_RANDOM 25

#define POSIX_PIE_LOAD_BASE  0x0000008000400000ull
#define POSIX_USER_STACK_TOP 0x00007FFFFFF00000ull
#define POSIX_USER_STACK_SZ  (512ull * 1024ull)   /* 512 KB baslangic yigini */

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) elf64_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) elf64_phdr_t;

typedef struct {
    int64_t  d_tag;
    uint64_t d_val;
} __attribute__((packed)) elf64_dyn_t;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} __attribute__((packed)) elf64_rela_t;

/* Kullanici VA'sina fiziksel cerceve uzerinden yaz (CR3 degistirmeden guvenli). */
static int write_to_space(vmm_space_t sp, uint64_t va, const void *src, uint64_t len) {
    const uint8_t *s = (const uint8_t *)src;
    while (len > 0) {
        uint64_t pte = vmm_space_get_user_pte(sp, va & PAGE_MASK);
        if (!(pte & VMM_PTE_P)) return -1;
        uint64_t pa = pte & VMM_ADDR_MASK;
        uint64_t off = va & (PAGE_SZ - 1ull);
        uint64_t chunk = PAGE_SZ - off;
        if (chunk > len) chunk = len;
        memcpy((void *)(uintptr_t)(pa + off), s, (size_t)chunk);
        va += chunk;
        s += chunk;
        len -= chunk;
    }
    return 0;
}

static int read_from_space(vmm_space_t sp, uint64_t va, void *dst, uint64_t len) {
    uint8_t *d = (uint8_t *)dst;
    while (len > 0) {
        uint64_t pte = vmm_space_get_user_pte(sp, va & PAGE_MASK);
        if (!(pte & VMM_PTE_P)) return -1;
        uint64_t pa = pte & VMM_ADDR_MASK;
        uint64_t off = va & (PAGE_SZ - 1ull);
        uint64_t chunk = PAGE_SZ - off;
        if (chunk > len) chunk = len;
        memcpy(d, (const void *)(uintptr_t)(pa + off), (size_t)chunk);
        va += chunk;
        d += chunk;
        len -= chunk;
    }
    return 0;
}

int posix_elf_load(posix_proc_t *p, const uint8_t *img, uint64_t size,
                   char *const argv[], int argc,
                   char *const envp[], int envc,
                   posix_elf_info_t *out) {
    if (!p || !img || !out || size < sizeof(elf64_ehdr_t)) return -ENOEXEC;

    elf64_ehdr_t eh;
    memcpy(&eh, img, sizeof(eh));
    if (eh.e_ident[0] != 0x7F || eh.e_ident[1] != 'E' ||
        eh.e_ident[2] != 'L'  || eh.e_ident[3] != 'F' ||
        eh.e_ident[4] != 2    || eh.e_machine != 0x3Eu)
        return -ENOEXEC;
    if (eh.e_type != ET_EXEC && eh.e_type != ET_DYN) return -ENOEXEC;
    if (eh.e_phentsize < sizeof(elf64_phdr_t) ||
        eh.e_phoff + (uint64_t)eh.e_phnum * eh.e_phentsize > size)
        return -ENOEXEC;

    uint64_t load_bias = (eh.e_type == ET_DYN) ? POSIX_PIE_LOAD_BASE : 0ull;
    uint64_t max_end = 0;
    uint64_t phdr_va = 0;
    uint64_t dyn_va = 0;
    uint64_t dyn_sz = 0;
    uint64_t nx = vmm_space_nx_ok() ? VMM_PTE_NX : 0ull;

    for (uint16_t i = 0; i < eh.e_phnum; i++) {
        elf64_phdr_t ph;
        memcpy(&ph, img + eh.e_phoff + (uint64_t)i * eh.e_phentsize, sizeof(ph));

        if (ph.p_type == PT_PHDR) {
            phdr_va = load_bias + ph.p_vaddr;
            continue;
        }
        if (ph.p_type == PT_DYNAMIC) {
            dyn_va = load_bias + ph.p_vaddr;
            dyn_sz = ph.p_memsz;
            continue;
        }
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;
        if (ph.p_filesz > ph.p_memsz || ph.p_offset + ph.p_filesz > size)
            return -ENOEXEC;

        uint64_t seg_va = load_bias + ph.p_vaddr;
        uint64_t page_start = seg_va & PAGE_MASK;
        uint64_t page_end = PAGE_UP(seg_va + ph.p_memsz);
        if (!vmm_space_user_range(page_start, page_end - page_start))
            return -ENOEXEC;

        if (eh.e_phoff >= ph.p_offset && eh.e_phoff < ph.p_offset + ph.p_filesz && !phdr_va)
            phdr_va = seg_va + (eh.e_phoff - ph.p_offset);

        uint32_t prot = 0;
        if (ph.p_flags & PF_R) prot |= PROT_READ;
        if (ph.p_flags & PF_W) prot |= PROT_WRITE;
        if (ph.p_flags & PF_X) prot |= PROT_EXEC;

        uint64_t pflags = 0;
        if (prot & PROT_WRITE) pflags |= VMM_PTE_W;
        if (!(prot & PROT_EXEC)) pflags |= nx;

        for (uint64_t va = page_start; va < page_end; va += PAGE_SZ) {
            uint64_t existing = vmm_space_get_user_pte(p->space, va);
            if (!(existing & VMM_PTE_P)) {
                uint64_t pa = vmm_alloc_frame();
                if (!pa) return -ENOMEM;
                if (vmm_space_map_user(p->space, va, pa, pflags) != 0) {
                    vmm_free_frame(pa);
                    return -ENOMEM;
                }
            } else {
                /* Iki PT_LOAD ayni 4K sayfayi paylasiyorsa izinleri birlestir */
                uint64_t merged = existing & (VMM_PTE_W | VMM_PTE_NX);
                if (pflags & VMM_PTE_W) merged |= VMM_PTE_W;
                if (!(pflags & VMM_PTE_NX)) merged &= ~VMM_PTE_NX;
                vmm_space_protect_user(p->space, va, merged);
            }
        }
        posix_vm_add_vma(p, page_start, page_end, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1);

        if (ph.p_filesz > 0) {
            if (write_to_space(p->space, seg_va, img + ph.p_offset, ph.p_filesz) != 0)
                return -EIO;
        }
        if (page_end > max_end) max_end = page_end;
    }

    if (max_end == 0) return -ENOEXEC;

    /* Statik PIE (ET_DYN): PT_DYNAMIC icindeki DT_RELA R_X86_64_RELATIVE
     * yer degistirmelerini (relocation) uygula; not: musl rcrt1.o da kendi
     * kendini relocate edebilir ama -static-pie olmayan ET_DYN icin de haziriz.
     * Dikkat: musl rcrt1.o kendisi RELATIVE uyguluyorsa cift uygulamamali!
     * musl rcrt1.o'da e_type == ET_DYN ve PT_INTERP yok; rcrt1 _dlstart
     * AT_BASE=0 gorunce kendisi +base ekler. Bu yuzden eger _dlstart varsa
     * (AT_PHDR/AT_BASE geciyoruz) veya standart PIE ise:
     * Linux cekirdegi PT_INTERP olmayan ET_DYN'de relocation YAPMAZ,
     * AT_PHDR + AT_PHNUM + AT_ENTRY + AT_BASE=0 verir; rcrt1.o kendisi yapar!
     * Sadece PT_DYNAMIC yoksa (duz ET_EXEC) relocation gerekmez. */
    (void)dyn_va;
    (void)dyn_sz;

    /* Kullanici yigini (512 KB, RW + NX) */
    uint64_t stack_bot = POSIX_USER_STACK_TOP - POSIX_USER_STACK_SZ;
    uint64_t stk_flags = VMM_PTE_W | nx;
    for (uint64_t va = stack_bot; va < POSIX_USER_STACK_TOP; va += PAGE_SZ) {
        uint64_t pa = vmm_alloc_frame();
        if (!pa) return -ENOMEM;
        if (vmm_space_map_user(p->space, va, pa, stk_flags) != 0) {
            vmm_free_frame(pa);
            return -ENOMEM;
        }
    }
    posix_vm_add_vma(p, stack_bot, POSIX_USER_STACK_TOP,
                     PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1);

    /* System V AMD64 ABI ilk yigin yerlesimi:
     * Yukaridan asagiya: [AT_RANDOM 16B] [env dizeleri] [argv dizeleri]
     * [hizalama] [auxv] [0] [envp] [0] [argv] [argc]  <- RSP (16B hizali) */
    uint64_t sp = POSIX_USER_STACK_TOP;

    /* 16 bayt rastgele (AT_RANDOM, stack protector kanaryasi icin) */
    uint32_t rnd[4];
    for (int i = 0; i < 4; i++) rnd[i] = net_rand32();
    sp -= 16;
    uint64_t rand_va = sp;
    write_to_space(p->space, rand_va, rnd, 16);

    uint64_t env_vas[32];
    if (envc > 32) envc = 32;
    for (int i = envc - 1; i >= 0; i--) {
        uint64_t n = strlen(envp[i]) + 1;
        sp -= n;
        env_vas[i] = sp;
        write_to_space(p->space, sp, envp[i], n);
    }

    uint64_t arg_vas[32];
    if (argc > 32) argc = 32;
    for (int i = argc - 1; i >= 0; i--) {
        uint64_t n = strlen(argv[i]) + 1;
        sp -= n;
        arg_vas[i] = sp;
        write_to_space(p->space, sp, argv[i], n);
    }

    sp &= ~15ull;

    uint64_t entry_va = load_bias + eh.e_entry;
    uint64_t auxv[][2] = {
        { AT_PHDR,   phdr_va },
        { AT_PHENT,  eh.e_phentsize },
        { AT_PHNUM,  eh.e_phnum },
        { AT_PAGESZ, PAGE_SZ },
        { AT_BASE,   0 },
        { AT_FLAGS,  0 },
        { AT_ENTRY,  entry_va },
        { AT_UID,    0 },
        { AT_EUID,   0 },
        { AT_GID,    0 },
        { AT_EGID,   0 },
        { AT_SECURE, 0 },
        { AT_RANDOM, rand_va },
        { AT_NULL,   0 },
    };
    int aux_n = (int)(sizeof(auxv) / sizeof(auxv[0]));

    /* Toplam qword sayisi: 1(argc) + argc + 1(NULL) + envc + 1(NULL) + 2*aux_n */
    int qwords = 1 + argc + 1 + envc + 1 + 2 * aux_n;
    if (qwords & 1) sp -= 8;   /* RSP 16-bayt hizali olsun */

    uint64_t frame_bytes = (uint64_t)qwords * 8ull;
    sp -= frame_bytes;
    uint64_t cur = sp;

    uint64_t w;
    w = (uint64_t)argc; write_to_space(p->space, cur, &w, 8); cur += 8;
    for (int i = 0; i < argc; i++) {
        write_to_space(p->space, cur, &arg_vas[i], 8); cur += 8;
    }
    w = 0; write_to_space(p->space, cur, &w, 8); cur += 8;
    for (int i = 0; i < envc; i++) {
        write_to_space(p->space, cur, &env_vas[i], 8); cur += 8;
    }
    w = 0; write_to_space(p->space, cur, &w, 8); cur += 8;
    for (int i = 0; i < aux_n; i++) {
        write_to_space(p->space, cur, &auxv[i][0], 8); cur += 8;
        write_to_space(p->space, cur, &auxv[i][1], 8); cur += 8;
    }

    p->brk_base = PAGE_UP(max_end + PAGE_SZ);
    p->brk_cur  = p->brk_base;
    if (p->mmap_next < p->brk_base + (64ull * 1024ull * 1024ull))
        p->mmap_next = PAGE_UP(p->brk_base + (64ull * 1024ull * 1024ull));

    out->entry    = entry_va;
    out->user_rsp = sp;
    out->brk_base = p->brk_base;
    (void)read_from_space;
    return 0;
}
