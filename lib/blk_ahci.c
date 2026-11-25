/* AHCI (SATA / M.2-SATA) blok cihazi. 64-bit cok-PRDT scatter-gather DMA + polling. */
#include "blockdev.h"
#include "mmio.h"
#include "kernel.h"
#include "alloc.h"
#ifdef ARCTIAN_BLK_SELFTEST
#include "serial.h"
#define BDBG(...) serial_printf(__VA_ARGS__)
#else
#define BDBG(...) do { } while (0)
#endif

extern uint64_t blk_virt_to_phys(const void *ptr);

#define AHCI_MAX_PORT     8
#define AHCI_MAX_PRDT     64u
#define AHCI_CT_SIZE      (0x80u + AHCI_MAX_PRDT * 16u)   /* 1152 byte, 128B hizali */
#define AHCI_CT_STRIDE    1280u
#define AHCI_DMA_BYTES    (256u * 1024u)                  /* tek komutta azami 256 KiB */
#define AHCI_MAX_SECTORS  (AHCI_DMA_BYTES / 512u)         /* 512 sektor */
#define AHCI_PRDT_MAX_SEG (4u * 1024u * 1024u)            /* AHCI PRDT girdi limiti: 4 MiB */

typedef struct {
    uint64_t phys;
    uint32_t bytes;
} ahci_sg_t;

typedef struct {
    volatile uint32_t *p;    /* port register blogu */
    uint8_t *cl;             /* 1024 byte, 1024B hizali */
    uint8_t *fis;            /* 256 byte, 256B hizali */
    uint8_t *ct;             /* AHCI_CT_STRIDE, 128B hizali */
    int      port;
    int      s64a;           /* CAP.S64A: 64-bit DMA destegi */
} ahci_ctx_t;

static ahci_ctx_t s_ctx[AHCI_MAX_PORT];
static int        s_ctxn = 0;

/* Buyuk tamponlar BSS yerine kmalloc uzerinden tek seferde ayrilir;
 * main_kernel 0x200000 BSS sinirini zorlamaz. */
static uint8_t   *s_dma_pool = 0;
static uint8_t   *s_bounce = 0;      /* AHCI_DMA_BYTES, 4096B hizali */

static void *ahci_alloc_aligned(size_t size, size_t align) {
    uintptr_t raw = (uintptr_t)kmalloc(size + align);
    if (!raw) return 0;
    uintptr_t aligned = (raw + align - 1u) & ~(align - 1u);
    memset((void *)aligned, 0, size);
    return (void *)aligned;
}

static int ahci_ensure_pool(void) {
    if (s_dma_pool && s_bounce) return 0;
    size_t per_port = 1024u + 256u + AHCI_CT_STRIDE; /* 2560 byte, hepsi >=128B hizali */
    if (!s_dma_pool) {
        s_dma_pool = (uint8_t *)ahci_alloc_aligned(AHCI_MAX_PORT * per_port, 4096u);
        if (!s_dma_pool) return -1;
    }
    if (!s_bounce) {
        s_bounce = (uint8_t *)ahci_alloc_aligned(AHCI_DMA_BYTES, 4096u);
        if (!s_bounce) return -1;
    }
    return 0;
}

static int ahci_port_stop(volatile uint32_t *p) {
    p[0x18/4] &= ~1u;
    long i;
    for (i = 0; i < 1000000L; i++) if (!(p[0x18/4] & (1u << 15))) break;
    p[0x18/4] &= ~(1u << 4);
    for (i = 0; i < 1000000L; i++) if (!(p[0x18/4] & (1u << 14))) break;
    return 0;
}

static int ahci_cmd_sg(ahci_ctx_t *c, int write, uint8_t *fis, int fis_dw,
                       const ahci_sg_t *sg, int sg_n) {
    volatile uint32_t *p = c->p;
    long t;

    if (sg_n < 0 || sg_n > (int)AHCI_MAX_PRDT) return -1;

    /* bosta bekle */
    for (t = 0; t < 2000000L; t++) {
        if (!(p[0x38/4] & 1u) && !(p[0x20/4] & 0x88u)) break;
    }
    p[0x10/4] = 0xFFFFFFFFu;  /* PxIS temizle */

    uint32_t *h = (uint32_t *)c->cl;
    uint64_t ct_pa = blk_virt_to_phys(c->ct);
    if (!ct_pa) return -1;

    h[0] = (uint32_t)(fis_dw & 0x1F);
    if (write) h[0] |= (1u << 6);
    h[0] |= ((uint32_t)sg_n << 16);   /* PRDTL: DW0 bit 16-31 */
    h[1] = 0;                          /* PRDBC */
    h[2] = (uint32_t)(ct_pa & 0xFFFFFFFFu);
    h[3] = c->s64a ? (uint32_t)(ct_pa >> 32) : 0u;

    memcpy(c->ct, fis, (size_t)fis_dw * 4u);

    uint32_t *prd = (uint32_t *)(c->ct + 0x80);
    for (int i = 0; i < sg_n; i++) {
        uint64_t pa = sg[i].phys;
        uint32_t nb = sg[i].bytes;
        if (!pa || nb == 0 || (nb & 1u) || nb > AHCI_PRDT_MAX_SEG) return -1;
        if (!c->s64a && (pa >> 32)) return -1;
        prd[i * 4 + 0] = (uint32_t)(pa & 0xFFFFFFFFu);
        prd[i * 4 + 1] = c->s64a ? (uint32_t)(pa >> 32) : 0u;
        prd[i * 4 + 2] = 0u;
        prd[i * 4 + 3] = ((nb - 1u) & 0x3FFFFFu) | ((i == sg_n - 1) ? (1u << 31) : 0u);
    }

    __asm__ volatile ("" ::: "memory");
    p[0x38/4] = 1u;   /* slot 0 baslat */

    int err = 0;
    const uint32_t err_mask = (1u << 30) | (1u << 29) | (1u << 28) | (1u << 27) | (1u << 26);
    for (t = 0; t < 10000000L; t++) {
        uint32_t is = p[0x10/4];
        if (is & err_mask) {   /* TFES/HBFS/HBDS/IFS/INFS */
            BDBG("[ahci] is=%08X tfd=%08X serr=%08X\n", is, p[0x20/4], p[0x30/4]);
            p[0x10/4] = is;
            err = -1;
            break;
        }
        if (!(p[0x38/4] & 1u)) break;
    }
    if ((p[0x38/4] & 1u) && !err) {
        BDBG("[ahci] timeout ci=%08X\n", p[0x38/4]);
        err = -1;
    }
    p[0x10/4] = p[0x10/4];
    return err;
}

static int ahci_cmd(ahci_ctx_t *c, int write, uint8_t *fis, int fis_dw,
                    void *buf, uint32_t bytes) {
    if (bytes == 0 || !buf)
        return ahci_cmd_sg(c, write, fis, fis_dw, 0, 0);
    uint64_t pa = blk_virt_to_phys(buf);
    if (!pa || (pa & 1u) || (!c->s64a && (pa >> 32))) return -1;
    ahci_sg_t sg = { pa, bytes };
    return ahci_cmd_sg(c, write, fis, fis_dw, &sg, 1);
}

static void ahci_fis(uint8_t *f, uint8_t cmd, uint64_t lba, uint16_t count) {
    for (int i = 0; i < 20; i++) f[i] = 0;
    f[0] = 0x27;
    f[1] = 0x80;
    f[2] = cmd;
    f[4] = (uint8_t)lba;
    f[5] = (uint8_t)(lba >> 8);
    f[6] = (uint8_t)(lba >> 16);
    f[7] = 0xE0;   /* LBA + master, bit7=1 (ATA zorunlu) */
    f[8] = (uint8_t)(lba >> 24);
    f[9] = (uint8_t)(lba >> 32);
    f[10] = (uint8_t)(lba >> 40);
    f[12] = (uint8_t)count;
    f[13] = (uint8_t)(count >> 8);
}

/* Sanal bellek tamponundan cok-PRDT scatter-gather listesi olusturur.
 * Fiziksel olarak bitisik sayfalar tek PRDT girdisinde birlestirilir.
 * Hizasiz veya haritasiz tamponlarda 0 doner (bounce yedegine dusulur). */
static uint32_t ahci_build_sg(ahci_ctx_t *c, const void *buf, uint32_t max_bytes,
                              ahci_sg_t *sg, int *sg_n_out) {
    uintptr_t va = (uintptr_t)buf;
    if (va & 1u) return 0;   /* AHCI DBA bit 0 == 0 zorunludur */

    int n = 0;
    uint32_t covered = 0;
    while (covered < max_bytes) {
        uint64_t pa = blk_virt_to_phys((const void *)(va + covered));
        if (!pa || (pa & 1u) || (!c->s64a && (pa >> 32))) break;
        uint32_t page_rem = 4096u - (uint32_t)((va + covered) & 0xFFFu);
        uint32_t seg_len = max_bytes - covered;
        if (seg_len > page_rem) seg_len = page_rem;

        if (n > 0 &&
            sg[n - 1].phys + sg[n - 1].bytes == pa &&
            sg[n - 1].bytes + seg_len <= AHCI_PRDT_MAX_SEG) {
            sg[n - 1].bytes += seg_len;
        } else {
            if (n >= (int)AHCI_MAX_PRDT) break;
            sg[n].phys = pa;
            sg[n].bytes = seg_len;
            n++;
        }
        covered += seg_len;
    }

    /* Sektor sinirina asagi yuvarla; AHCI ATA komutu tam sektor aktarir. */
    uint32_t sec_bytes = (covered / 512u) * 512u;
    if (sec_bytes == 0 || n == 0) return 0;
    if (sec_bytes < covered) {
        uint32_t trim = covered - sec_bytes;
        while (trim > 0 && n > 0) {
            if (sg[n - 1].bytes > trim) {
                sg[n - 1].bytes -= trim;
                trim = 0;
            } else {
                trim -= sg[n - 1].bytes;
                n--;
            }
        }
    }
    *sg_n_out = n;
    return sec_bytes;
}

static int ahci_transfer(ahci_ctx_t *c, int write, uint64_t lba, uint32_t count,
                         void *buf) {
    if (count == 0) return 0;
    uint8_t *p = (uint8_t *)buf;
    ahci_sg_t sg[AHCI_MAX_PRDT];

    while (count) {
        uint32_t max_secs = count > AHCI_MAX_SECTORS ? AHCI_MAX_SECTORS : count;
        int sg_n = 0;
        uint32_t sg_bytes = ahci_build_sg(c, p, max_secs * 512u, sg, &sg_n);

        if (sg_bytes >= 512u && sg_n > 0) {
            uint32_t chunk = sg_bytes / 512u;
            uint8_t fis[20];
            ahci_fis(fis, write ? 0x35 : 0x25, lba, (uint16_t)chunk);
            if (ahci_cmd_sg(c, write, fis, 5, sg, sg_n) != 0) return -1;
            p += sg_bytes;
            lba += chunk;
            count -= chunk;
            continue;
        }

        /* Bounce yedegi: hizasiz veya 64-bit kisitli bellek */
        if (!s_bounce) return -1;
        uint32_t chunk = max_secs;
        uint32_t bytes = chunk * 512u;
        uint8_t fis[20];
        ahci_fis(fis, write ? 0x35 : 0x25, lba, (uint16_t)chunk);
        if (!write) {
            if (ahci_cmd(c, 0, fis, 5, s_bounce, bytes) != 0) return -1;
            memcpy(p, s_bounce, bytes);
        } else {
            memcpy(s_bounce, p, bytes);
            if (ahci_cmd(c, 1, fis, 5, s_bounce, bytes) != 0) return -1;
        }
        p += bytes;
        lba += chunk;
        count -= chunk;
    }
    return 0;
}

static int ahci_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    return ahci_transfer((ahci_ctx_t *)ctx, 0, lba, count, buf);
}
static int ahci_write(void *ctx, uint64_t lba, uint32_t count, const void *buf) {
    return ahci_transfer((ahci_ctx_t *)ctx, 1, lba, count, (void *)buf);
}
static int ahci_flush(void *ctx) {
    ahci_ctx_t *c = (ahci_ctx_t *)ctx;
    uint8_t fis[20];
    ahci_fis(fis, 0xE7, 0, 0);   /* FLUSH CACHE */
    return ahci_cmd(c, 0, fis, 5, 0, 0);
}

static int ahci_port_init(ahci_ctx_t *c) {
    volatile uint32_t *p = c->p;
    uint64_t cl_pa = blk_virt_to_phys(c->cl);
    uint64_t fis_pa = blk_virt_to_phys(c->fis);
    if (!cl_pa || !fis_pa) return -1;

    ahci_port_stop(p);
    p[0x00/4] = (uint32_t)(cl_pa & 0xFFFFFFFFu);   /* PxCLB */
    p[0x04/4] = c->s64a ? (uint32_t)(cl_pa >> 32) : 0u;
    p[0x08/4] = (uint32_t)(fis_pa & 0xFFFFFFFFu);  /* PxFB */
    p[0x0C/4] = c->s64a ? (uint32_t)(fis_pa >> 32) : 0u;
    p[0x10/4] = 0xFFFFFFFFu;
    p[0x30/4] = 0xFFFFFFFFu;
    memset(c->cl, 0, 1024);
    memset(c->fis, 0, 256);
    p[0x18/4] |= (1u << 4) | (1u << 2) | (1u << 1) | (1u << 28); /* FRE|POD|SUD|ICC */
    for (long t = 0; t < 1000000L; t++) if (!(p[0x18/4] & (1u << 14))) break;
    p[0x18/4] |= 1u;          /* ST  */
    BDBG("[ahci] clb=%08X:%08X fb=%08X:%08X s64a=%d cmd=%08X\n",
         p[0x04/4], p[0x00/4], p[0x0C/4], p[0x08/4], c->s64a, p[0x18/4]);
    return 0;
}

static int ahci_identify(ahci_ctx_t *c, uint64_t *sectors) {
    uint8_t fis[20];
    ahci_fis(fis, 0xEC, 0, 1);   /* IDENTIFY: sektor sayaci 1 (QEMU DMA kisiti) */
    fis[7] = 0xA0;
    for (int i = 0; i < 512; i++) s_bounce[i] = 0xAA;
    int rc = ahci_cmd(c, 0, fis, 5, s_bounce, 512);
    if (rc != 0) return -1;
    uint16_t *id = (uint16_t *)s_bounce;
    BDBG("[ahci] id[0]=%04X id[60]=%04X id[61]=%04X\n", id[0], id[60], id[61]);
    uint64_t lba48 = 0;
    if (id[83] & (1u << 10)) {
        lba48 = (uint64_t)id[100] | ((uint64_t)id[101] << 16) |
                ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    }
    uint32_t lba28 = ((uint32_t)id[61] << 16) | id[60];
    *sectors = lba48 ? lba48 : lba28;
    return 0;
}

#ifdef ARCTIAN_BLK_SELFTEST
static void ahci_selftest_port(ahci_ctx_t *c, uint64_t total_secs) {
    if (total_secs < 1024u) return;
    /* Tahribatsiz 16-PRDT scatter-gather + 128 KB toplu DMA dogrulamasi:
     * sondaki yedek bolgeden onceki veriyi yedekleyip geri yazar. */
    uint64_t test_lba = total_secs - 512u;
    const uint32_t nsecs = 256u;                 /* 128 KiB */
    const uint32_t nbytes = nsecs * 512u;
    uint8_t *save = (uint8_t *)ahci_alloc_aligned(nbytes, 4096u);
    uint8_t *wbuf = (uint8_t *)ahci_alloc_aligned(nbytes, 4096u);
    uint8_t *rbuf = (uint8_t *)ahci_alloc_aligned(nbytes, 4096u);
    if (!save || !wbuf || !rbuf) {
        BDBG("[ahci] selftest alloc fail\n");
        return;
    }

    if (ahci_read(c, test_lba, nsecs, save) != 0) {
        BDBG("[ahci] selftest save-read fail\n");
        goto done;
    }

    /* 1) 128 KiB tek FIS yazma + flush + okuma */
    for (uint32_t i = 0; i < nbytes; i++)
        wbuf[i] = (uint8_t)((i * 131u + 0x5Au) ^ (i >> 9));
    if (ahci_write(c, test_lba, nsecs, wbuf) != 0 || ahci_flush(c) != 0 ||
        ahci_read(c, test_lba, nsecs, rbuf) != 0 || memcmp(wbuf, rbuf, nbytes) != 0) {
        BDBG("[ahci] selftest 128KB rw-verify FAIL\n");
        goto restore;
    }

    /* 2) Acik 16-PRDT scatter-gather komutu (16 ayrik 4 KiB dilim = 64 KiB tek FIS) */
    ahci_sg_t sg16[16];
    for (int k = 0; k < 16; k++) {
        uint8_t *page = wbuf + (size_t)k * 8192u;  /* her iki sayfada bir */
        for (int b = 0; b < 4096; b++) page[b] = (uint8_t)(0xA0u ^ k ^ (b & 0xFF));
        sg16[k].phys = blk_virt_to_phys(page);
        sg16[k].bytes = 4096u;
    }
    uint8_t fis[20];
    ahci_fis(fis, 0x35, test_lba, 128u);         /* WRITE DMA EXT 64 KiB */
    if (ahci_cmd_sg(c, 1, fis, 5, sg16, 16) != 0 || ahci_flush(c) != 0) {
        BDBG("[ahci] selftest 16-PRDT write FAIL\n");
        goto restore;
    }
    /* Farkli 16 hedef sayfaya geri oku ve dogrula */
    ahci_sg_t rsg16[16];
    memset(rbuf, 0, nbytes);
    for (int k = 0; k < 16; k++) {
        rsg16[k].phys = blk_virt_to_phys(rbuf + (size_t)k * 8192u);
        rsg16[k].bytes = 4096u;
    }
    ahci_fis(fis, 0x25, test_lba, 128u);         /* READ DMA EXT 64 KiB */
    if (ahci_cmd_sg(c, 0, fis, 5, rsg16, 16) != 0) {
        BDBG("[ahci] selftest 16-PRDT read FAIL\n");
        goto restore;
    }
    int sg_ok = 1;
    for (int k = 0; k < 16; k++) {
        if (memcmp(wbuf + (size_t)k * 8192u, rbuf + (size_t)k * 8192u, 4096u) != 0) {
            sg_ok = 0;
            break;
        }
    }
    BDBG("[ahci] port=%d 128KB-bulk + 16-PRDT SG-DMA rw-verify %s\n",
         c->port, sg_ok ? "OK" : "FAIL");

restore:
    (void)ahci_write(c, test_lba, nsecs, save);
    (void)ahci_flush(c);
done:
    /* ahci_alloc_aligned hizali pointer dondurdugu icin free edilmez,
     * yalniz selftest modunda kosar ve 384 KB tek seferliktir. */
    (void)save;
}
#endif

void blk_ahci_scan(void) {
    BDBG("[ahci] scan\n");
    s_ctxn = 0;
    if (ahci_ensure_pool() != 0) return;

    const size_t per_port = 1024u + 256u + AHCI_CT_STRIDE;

    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint32_t id = blk_pci_readl((uint8_t)bus, (uint8_t)slot, 0, 0x08);
            uint8_t cc = (uint8_t)(id >> 24), sc = (uint8_t)(id >> 16), pi = (uint8_t)(id >> 8);
            if (cc != 0x01 || sc != 0x06 || pi != 0x01) continue;

            blk_pci_enable((uint8_t)bus, (uint8_t)slot, 0);
            uint64_t abar = blk_pci_bar((uint8_t)bus, (uint8_t)slot, 0, 5) & ~0xFULL;
            BDBG("[ahci] bus=%d slot=%d bar5=%llX\n", bus, slot, (unsigned long long)abar);
            if (!abar) continue;
            blk_mmio_map(abar, 0x2000);

            volatile uint32_t *hba = (volatile uint32_t *)(uintptr_t)abar;
            hba[0x04/4] |= (1u << 31);           /* AHCI enable */
            uint32_t cap = hba[0x00/4];
            uint32_t pi_ports = hba[0x0C/4];
            int s64a = (cap & (1u << 31)) ? 1 : 0;
            BDBG("[ahci] cap=%08X s64a=%d pi=%08X\n", cap, s64a, pi_ports);
            if (pi_ports == 0) pi_ports = 0x1;

            for (int pr = 0; pr < 32 && s_ctxn < AHCI_MAX_PORT; pr++) {
                if (!(pi_ports & (1u << pr))) continue;
                volatile uint32_t *pp = (volatile uint32_t *)(uintptr_t)(abar + 0x100 + 0x80 * pr);
                uint32_t ssts = pp[0x28/4];
                uint32_t sig = pp[0x24/4];
                BDBG("[ahci] port=%d ssts=%08X sig=%08X\n", pr, ssts, sig);
                if ((ssts & 0xF) != 3) continue;
                if (sig != 0x00000101) continue;   /* SATA disk */

                uint8_t *base = s_dma_pool + (size_t)s_ctxn * per_port;
                ahci_ctx_t *c = &s_ctx[s_ctxn];
                c->p = pp;
                c->cl = base;
                c->fis = base + 1024u;
                c->ct = base + 1024u + 256u;
                c->port = pr;
                c->s64a = s64a;

                if (ahci_port_init(c) != 0) continue;
                uint64_t secs = 0;
                int ir = ahci_identify(c, &secs);
                BDBG("[ahci] identify=%d secs=%u\n", ir, (unsigned)secs);
                if (ir != 0 || secs == 0) { ahci_port_stop(pp); continue; }

#ifdef ARCTIAN_BLK_SELFTEST
                ahci_selftest_port(c, secs);
#endif

                bdev_ops_t ops;
                for (int i = 0; i < 24; i++) ops.name[i] = 0;
                ops.name[0] = 's'; ops.name[1] = 'a'; ops.name[2] = 't'; ops.name[3] = 'a';
                ops.name[4] = (char)('0' + (pr % 10));
                ops.kind = BDEV_KIND_AHCI;
                ops.sectors = secs;
                ops.removable = 0;
                ops.ctx = c;
                ops.read = ahci_read;
                ops.write = ahci_write;
                ops.flush = ahci_flush;
                blockdev_register(&ops);
                s_ctxn++;
            }
        }
    }
}
