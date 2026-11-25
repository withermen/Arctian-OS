/* POSIX ELF64 akisli (streaming) yukleyici:
 *   - PT_LOAD + statik PIE (ET_DYN + PT_DYNAMIC) + ET_EXEC
 *   - Kucuk cekirdek tamponu (256 KB) ile AFS'den dogrudan Ring-3 sayfalarina
 *     akisli yukleme -> 40 MB - 128 MB+ ELF64 ikilileri (JSC/WebKit) 16 MB
 *     siniri ve dev kmalloc olmadan yuklenir.
 *   - 4 MB kullanici yigini (ust 64 KB hazir, kalan ~3.94 MB demand-zeroed).
 *   - System V AMD64 ABI ilk yigin duzeni (argc, argv, envp, auxv).
 */
#include "posix.h"
#include "alloc.h"
#include "afs.h"
#include "clock.h"
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

#define POSIX_PIE_LOAD_BASE   0x0000008000400000ull
#define POSIX_USER_STACK_TOP  0x00007FFFFFF00000ull
#define POSIX_USER_STACK_SZ   POSIX_USER_STACK_SZ_BYTES    /* 4 MB */
#define POSIX_USER_STACK_INIT (64ull * 1024ull)            /* ilk 64 KB hazir */

#define ELF_STREAM_BUF_SZ     (256u * 1024u)
#define ELF_STREAM_BUF_MIN    (64u * 1024u)
#define ELF_MAX_STACK_ARGS    64
#define ELF_MAX_STACK_ENVS    64

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
    int64_t (*read_at)(void *ctx, void *dst, uint64_t len, uint64_t off);
    void    *ctx;
    uint64_t size;
} posix_elf_src_t;

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

static int elf_load_stream(posix_proc_t *p, const posix_elf_src_t *src,
                           char *const argv[], int argc,
                           char *const envp[], int envc,
                           posix_elf_info_t *out) {
    if (!p || !src || !src->read_at || !out || src->size < sizeof(elf64_ehdr_t))
        return -ENOEXEC;

    elf64_ehdr_t eh;
    if (src->read_at(src->ctx, &eh, sizeof(eh), 0) != (int64_t)sizeof(eh))
        return -EIO;
    if (eh.e_ident[0] != 0x7F || eh.e_ident[1] != 'E' ||
        eh.e_ident[2] != 'L'  || eh.e_ident[3] != 'F' ||
        eh.e_ident[4] != 2    || eh.e_machine != 0x3Eu)
        return -ENOEXEC;
    if (eh.e_type != ET_EXEC && eh.e_type != ET_DYN) return -ENOEXEC;
    if (eh.e_phnum == 0 || eh.e_phnum > 128u ||
        eh.e_phentsize < sizeof(elf64_phdr_t) ||
        eh.e_phoff + (uint64_t)eh.e_phnum * eh.e_phentsize > src->size)
        return -ENOEXEC;

    /* Program baslik tablosunu kucuk gecici tampona oku */
    uint64_t ph_bytes = (uint64_t)eh.e_phnum * eh.e_phentsize;
    uint8_t *ph_buf = (uint8_t *)kmalloc((size_t)ph_bytes);
    if (!ph_buf) return -ENOMEM;
    if (src->read_at(src->ctx, ph_buf, ph_bytes, eh.e_phoff) != (int64_t)ph_bytes) {
        kfree(ph_buf);
        return -EIO;
    }

    /* Akisli kopya tamponu (256 KB, heap darsa 64 KB yedek) */
    uint64_t bounce_sz = ELF_STREAM_BUF_SZ;
    uint8_t *bounce = (uint8_t *)kmalloc((size_t)bounce_sz);
    if (!bounce) {
        bounce_sz = ELF_STREAM_BUF_MIN;
        bounce = (uint8_t *)kmalloc((size_t)bounce_sz);
        if (!bounce) {
            kfree(ph_buf);
            return -ENOMEM;
        }
    }

    uint64_t load_bias = (eh.e_type == ET_DYN) ? POSIX_PIE_LOAD_BASE : 0ull;
    uint64_t max_end = 0;
    uint64_t phdr_va = 0;
    uint64_t nx = vmm_space_nx_ok() ? VMM_PTE_NX : 0ull;
    int rc = 0;

    uint64_t t0_ms = clock_ms();
    if (src->size > (4ull * 1024ull * 1024ull))
        serial_printf("[elf] stream load start size=%u KB\n", (unsigned)(src->size / 1024ull));

    for (uint16_t i = 0; i < eh.e_phnum; i++) {
        elf64_phdr_t ph;
        memcpy(&ph, ph_buf + (uint64_t)i * eh.e_phentsize, sizeof(ph));

        if (ph.p_type == PT_PHDR) {
            phdr_va = load_bias + ph.p_vaddr;
            continue;
        }
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;
        if (ph.p_filesz > ph.p_memsz || ph.p_offset + ph.p_filesz > src->size) {
            rc = -ENOEXEC;
            break;
        }

        uint64_t seg_va = load_bias + ph.p_vaddr;
        uint64_t page_start = seg_va & PAGE_MASK;
        uint64_t page_end = PAGE_UP(seg_va + ph.p_memsz);
        if (!vmm_space_user_range(page_start, page_end - page_start)) {
            rc = -ENOEXEC;
            break;
        }

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
                if (!pa) { rc = -ENOMEM; break; }
                if (vmm_space_map_user(p->space, va, pa, pflags) != 0) {
                    vmm_free_frame(pa);
                    rc = -ENOMEM;
                    break;
                }
            } else {
                uint64_t merged = existing & (VMM_PTE_W | VMM_PTE_NX);
                if (pflags & VMM_PTE_W) merged |= VMM_PTE_W;
                if (!(pflags & VMM_PTE_NX)) merged &= ~VMM_PTE_NX;
                vmm_space_protect_user(p->space, va, merged);
            }
        }
        if (rc < 0) break;

        if (posix_vm_add_vma(p, page_start, page_end, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1) < 0) {
            rc = -ENOMEM;
            break;
        }

        /* PT_LOAD dosya icerigini bounce tamponu ile parcali olarak sayfalara akisli yaz */
        uint64_t done = 0;
        while (done < ph.p_filesz) {
            uint64_t chunk = ph.p_filesz - done;
            if (chunk > bounce_sz) chunk = bounce_sz;
            int64_t got = src->read_at(src->ctx, bounce, chunk, ph.p_offset + done);
            if (got != (int64_t)chunk) { rc = -EIO; break; }
            if (write_to_space(p->space, seg_va + done, bounce, chunk) != 0) {
                rc = -EIO;
                break;
            }
            done += chunk;
        }
        if (rc < 0) break;

        if (page_end > max_end) max_end = page_end;
    }

    kfree(bounce);
    kfree(ph_buf);
    if (rc < 0) return rc;
    if (max_end == 0) return -ENOEXEC;

    /* 4 MB kullanici yigini (RW + NX): ust 64 KB hemen eslenir, alt kisim
     * ilk erisimde demand-zeroed #PF ile tahsis edilir. */
    uint64_t stack_bot  = POSIX_USER_STACK_TOP - POSIX_USER_STACK_SZ;
    uint64_t stack_init = POSIX_USER_STACK_TOP - POSIX_USER_STACK_INIT;
    uint64_t stk_flags  = VMM_PTE_W | nx;
    for (uint64_t va = stack_init; va < POSIX_USER_STACK_TOP; va += PAGE_SZ) {
        uint64_t pa = vmm_alloc_frame();
        if (!pa) return -ENOMEM;
        if (vmm_space_map_user(p->space, va, pa, stk_flags) != 0) {
            vmm_free_frame(pa);
            return -ENOMEM;
        }
    }
    if (posix_vm_add_vma(p, stack_bot, POSIX_USER_STACK_TOP,
                         PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1) < 0)
        return -ENOMEM;

    /* System V AMD64 ABI ilk yigin yerlesimi */
    uint64_t sp = POSIX_USER_STACK_TOP;

    uint32_t rnd[4];
    for (int i = 0; i < 4; i++) rnd[i] = net_rand32();
    sp -= 16;
    uint64_t rand_va = sp;
    write_to_space(p->space, rand_va, rnd, 16);

    uint64_t env_vas[ELF_MAX_STACK_ENVS];
    if (envc > ELF_MAX_STACK_ENVS) envc = ELF_MAX_STACK_ENVS;
    for (int i = envc - 1; i >= 0; i--) {
        uint64_t n = strlen(envp[i]) + 1;
        sp -= n;
        env_vas[i] = sp;
        write_to_space(p->space, sp, envp[i], n);
    }

    uint64_t arg_vas[ELF_MAX_STACK_ARGS];
    if (argc > ELF_MAX_STACK_ARGS) argc = ELF_MAX_STACK_ARGS;
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

    int qwords = 1 + argc + 1 + envc + 1 + 2 * aux_n;
    if (qwords & 1) sp -= 8;

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
    if (src->size > (4ull * 1024ull * 1024ull))
        serial_printf("[elf] stream load OK in %u ms entry=0x%X%08X rsp=0x%X%08X\n",
                      (unsigned)(clock_ms() - t0_ms),
                      (unsigned)(entry_va >> 32), (unsigned)entry_va,
                      (unsigned)(sp >> 32), (unsigned)sp);
    return 0;
}

typedef struct {
    const uint8_t *img;
    uint64_t       size;
} mem_src_ctx_t;

static int64_t mem_src_read_at(void *ctx, void *dst, uint64_t len, uint64_t off) {
    mem_src_ctx_t *m = (mem_src_ctx_t *)ctx;
    if (off >= m->size) return 0;
    uint64_t avail = m->size - off;
    if (len > avail) len = avail;
    memcpy(dst, m->img + off, (size_t)len);
    return (int64_t)len;
}

static int64_t afs_src_read_at(void *ctx, void *dst, uint64_t len, uint64_t off) {
    if (off > 0xFFFFFFFFull || len > 0xFFFFFFFFull) return -1;
    uint32_t got = afs_read_at(ctx, dst, (uint32_t)len, (uint32_t)off);
    return (int64_t)got;
}

int posix_elf_load(posix_proc_t *p, const uint8_t *img, uint64_t size,
                   char *const argv[], int argc,
                   char *const envp[], int envc,
                   posix_elf_info_t *out) {
    if (!img) return -ENOEXEC;
    mem_src_ctx_t m = { img, size };
    posix_elf_src_t src = { mem_src_read_at, &m, size };
    return elf_load_stream(p, &src, argv, argc, envp, envc, out);
}

int posix_elf_load_afs(posix_proc_t *p, void *afs_handle, uint64_t size,
                       char *const argv[], int argc,
                       char *const envp[], int envc,
                       posix_elf_info_t *out) {
    if (!afs_handle) return -ENOENT;
    posix_elf_src_t src = { afs_src_read_at, afs_handle, size };
    return elf_load_stream(p, &src, argv, argc, envp, envc, out);
}
