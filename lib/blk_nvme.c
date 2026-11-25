/* NVMe (M.2 / PCIe SSD) blok cihazi. 64-bit PRP1/PRP2/PRP-List DMA + polling. */
#include "blockdev.h"
#include "mmio.h"
#include "kernel.h"
#include "alloc.h"
#ifdef ARCTIAN_BLK_SELFTEST
#include "serial.h"
#define NDBG(...) serial_printf(__VA_ARGS__)
#else
#define NDBG(...) do { } while (0)
#endif

extern uint64_t blk_virt_to_phys(const void *ptr);

#define NVME_AQ_ENTRIES 16
#define NVME_IO_ENTRIES 64
#define NVME_PAGE       4096u
#define NVME_MAX_XFER   (256u * 1024u)   /* tek komutta azami 256 KiB (64 sayfa) */
#define NVME_BOUNCE_SZ  (64u * 1024u)    /* hizasiz tamponlar icin 64 KiB yedek */

/* Kuyruklar ve PRP listesi BSS yerine kmalloc ile 4K hizali ayrilir. */
static uint32_t *s_asq = 0;
static uint32_t *s_acq = 0;
static uint32_t *s_iosq = 0;
static uint32_t *s_iocq = 0;
static uint64_t *s_prp_list = 0;        /* 512 x uint64_t = 4096 byte */
static uint8_t  *s_bounce = 0;          /* NVME_BOUNCE_SZ */
static uint8_t  *s_ident = 0;           /* 4096 byte */

static volatile uint32_t *g_mmio;
static uint32_t g_stride;
static int      g_phase = 1;
static uint32_t g_asq_tail, g_acq_head;
static uint32_t g_sq_tail, g_cq_head;
static int      g_io_phase = 1;
static uint32_t g_nsid = 1;
static uint32_t g_lba_shift = 9;        /* 512-byte varsayilan */
static uint64_t g_nsze = 0;
static uint32_t g_max_xfer = NVME_MAX_XFER;
static uint16_t g_io_cid = 1;
static int      g_ready = 0;

#define NVME_REG_CAP   0x00
#define NVME_REG_INTMS 0x0C
#define NVME_REG_CC    0x14
#define NVME_REG_CST   0x1C
#define NVME_REG_AQA   0x24
#define NVME_REG_ASQ   0x28
#define NVME_REG_ACQ   0x30

static void *nvme_alloc_page(size_t bytes) {
    uintptr_t raw = (uintptr_t)kmalloc(bytes + NVME_PAGE);
    if (!raw) return 0;
    uintptr_t p = (raw + NVME_PAGE - 1u) & ~(uintptr_t)(NVME_PAGE - 1u);
    memset((void *)p, 0, bytes);
    return (void *)p;
}

static int nvme_ensure_mem(void) {
    if (!s_asq) s_asq = (uint32_t *)nvme_alloc_page(NVME_AQ_ENTRIES * 64u);
    if (!s_acq) s_acq = (uint32_t *)nvme_alloc_page(NVME_AQ_ENTRIES * 16u);
    if (!s_iosq) s_iosq = (uint32_t *)nvme_alloc_page(NVME_IO_ENTRIES * 64u);
    if (!s_iocq) s_iocq = (uint32_t *)nvme_alloc_page(NVME_IO_ENTRIES * 16u);
    if (!s_prp_list) s_prp_list = (uint64_t *)nvme_alloc_page(NVME_PAGE);
    if (!s_bounce) s_bounce = (uint8_t *)nvme_alloc_page(NVME_BOUNCE_SZ);
    if (!s_ident) s_ident = (uint8_t *)nvme_alloc_page(NVME_PAGE);
    return (s_asq && s_acq && s_iosq && s_iocq && s_prp_list && s_bounce && s_ident) ? 0 : -1;
}

static uint32_t nvme_rd(uint32_t off) { return g_mmio[off / 4]; }
static void nvme_wr(uint32_t off, uint32_t v) { g_mmio[off / 4] = v; }

static void nvme_doorbell(uint32_t qid, uint32_t tail, int is_cq) {
    uint32_t off = 0x1000 + (qid * 2 + (is_cq ? 1 : 0)) * g_stride;
    g_mmio[off / 4] = tail;
}

static int nvme_wait_ready(int ready) {
    for (long t = 0; t < 10000000L; t++) {
        uint32_t cst = nvme_rd(NVME_REG_CST);
        if (ready ? (cst & 1) : !(cst & 1)) return 0;
    }
    return -1;
}

static void nvme_admin_submit(uint32_t *sqe) {
    uint32_t *slot = &s_asq[g_asq_tail * 16];
    for (int i = 0; i < 16; i++) slot[i] = sqe[i];
    g_asq_tail = (g_asq_tail + 1) % NVME_AQ_ENTRIES;
    __asm__ volatile ("" ::: "memory");
    nvme_doorbell(0, g_asq_tail, 0);
}

static int nvme_admin_reap(uint32_t *result, long wait_iters) {
    for (long t = 0; t < wait_iters; t++) {
        if ((t & 0xFF) == 0) outb(0x80, 0);
        volatile uint32_t *cqe = &s_acq[g_acq_head * 4];
        uint32_t dw3 = cqe[3];
        uint16_t sf = (uint16_t)(dw3 >> 16);
        if ((int)(sf & 1) == g_phase) {
            uint32_t status = (sf >> 1) & 0x7FFF;
            if (result) *result = cqe[0];
            g_acq_head = (g_acq_head + 1) % NVME_AQ_ENTRIES;
            if (g_acq_head == 0) g_phase ^= 1;
            nvme_doorbell(0, g_acq_head, 1);
            return status == 0 ? 0 : -1;
        }
    }
    return -1;
}

static int nvme_admin(uint32_t *sqe, uint32_t *result) {
    nvme_admin_submit(sqe);
    if (nvme_admin_reap(result, 4000000L) == 0) return 0;
    NDBG("[nvme] admin fail phase=%d head=%d tail=%d\n", g_phase, g_acq_head, g_asq_tail);
    return -1;
}

static int nvme_io_once(uint32_t *sqe, long wait_iters) {
    uint32_t *slot = &s_iosq[g_sq_tail * 16];
    for (int i = 0; i < 16; i++) slot[i] = sqe[i];
    g_sq_tail = (g_sq_tail + 1) % NVME_IO_ENTRIES;
    __asm__ volatile ("" ::: "memory");
    nvme_doorbell(1, g_sq_tail, 0);

    for (long t = 0; t < wait_iters; t++) {
        if ((t & 0xFF) == 0) outb(0x80, 0);
        volatile uint32_t *cqe = &s_iocq[g_cq_head * 4];
        uint32_t dw3 = cqe[3];
        uint16_t sf = (uint16_t)(dw3 >> 16);
        if ((int)(sf & 1) == g_io_phase) {
            uint32_t status = (sf >> 1) & 0x7FFF;
            g_cq_head = (g_cq_head + 1) % NVME_IO_ENTRIES;
            if (g_cq_head == 0) g_io_phase ^= 1;
            nvme_doorbell(1, g_cq_head, 1);
            if (status != 0)
                NDBG("[nvme] io err op=%02X status=%04X\n", sqe[0] & 0xFFu, status);
            return status == 0 ? 0 : -1;
        }
    }
    return -1;
}

static int nvme_io(uint32_t *sqe) {
    return nvme_io_once(sqe, 4000000L);
}

static void nvme_mk_sqe(uint32_t *s, uint8_t opcode, uint32_t cid) {
    for (int i = 0; i < 16; i++) s[i] = 0;
    s[0] = (uint32_t)opcode | ((cid & 0xFFFFu) << 16);
}

/* 3 PRP durumunu da isler:
 *  - <= 1 sayfa  : yalniz PRP1 (PRP2 = 0)
 *  - 2 sayfa     : PRP1 + PRP2 (dogrudan 2. sayfa fiziksel adresi)
 *  - >= 3 sayfa  : PRP1 + PRP2 -> 4K hizali s_prp_list[] (64-bit adresler) */
static int nvme_fill_prp(uint32_t *sqe, const void *buf, uint32_t bytes) {
    uintptr_t va = (uintptr_t)buf;
    uint64_t pa0 = blk_virt_to_phys(buf);
    if (!pa0 || (pa0 & 3u)) return -1;   /* NVMe PRP DWORD hizali olmalidir */

    uint32_t off = (uint32_t)(pa0 & (NVME_PAGE - 1u));
    uint32_t total_pages = (off + bytes + NVME_PAGE - 1u) / NVME_PAGE;
    if (total_pages == 0 || total_pages > 513u) return -1;

    sqe[6] = (uint32_t)(pa0 & 0xFFFFFFFFu);
    sqe[7] = (uint32_t)(pa0 >> 32);

    if (total_pages == 1u) {
        sqe[8] = 0u;
        sqe[9] = 0u;
        return 0;
    }

    uint32_t first_len = NVME_PAGE - off;
    uint64_t pa1 = blk_virt_to_phys((const void *)(va + first_len));
    if (!pa1 || (pa1 & (NVME_PAGE - 1u))) return -1;

    if (total_pages == 2u) {
        sqe[8] = (uint32_t)(pa1 & 0xFFFFFFFFu);
        sqe[9] = (uint32_t)(pa1 >> 32);
        return 0;
    }

    /* >= 3 sayfa: PRP2 = PRP List sayfa fiziksel adresi */
    s_prp_list[0] = pa1;
    for (uint32_t p = 2u; p < total_pages; p++) {
        uint64_t pa = blk_virt_to_phys((const void *)(va + first_len + (p - 1u) * NVME_PAGE));
        if (!pa || (pa & (NVME_PAGE - 1u))) return -1;
        s_prp_list[p - 1u] = pa;
    }
    uint64_t list_pa = blk_virt_to_phys(s_prp_list);
    if (!list_pa || (list_pa & (NVME_PAGE - 1u))) return -1;
    sqe[8] = (uint32_t)(list_pa & 0xFFFFFFFFu);
    sqe[9] = (uint32_t)(list_pa >> 32);
    return 0;
}

static int nvme_transfer(uint64_t lba512, uint32_t count512, void *buf, int write) {
    if (count512 == 0) return 0;
    uint32_t sec_bytes = 1u << g_lba_shift;
    uint32_t per = sec_bytes / 512u;   /* 512-birim / NVMe blok */
    if (per == 0) per = 1;
    uint8_t *p = (uint8_t *)buf;

    while (count512) {
        if (lba512 % per) return -1;   /* NVMe bloguna hizasiz */
        uint64_t lba = lba512 / per;
        uint32_t avail = count512 / per;
        if (avail == 0) return -1;

        uint32_t max_blocks = g_max_xfer / sec_bytes;
        if (max_blocks == 0) max_blocks = 1;
        uint32_t blocks = (avail < max_blocks) ? avail : max_blocks;
        uint32_t chunk_bytes = blocks * sec_bytes;

        uint32_t s[16];
        g_io_cid = (uint16_t)((g_io_cid + 1u) & 0xFFFFu);
        nvme_mk_sqe(s, write ? 0x01 : 0x02, g_io_cid);
        s[1] = g_nsid;
        s[10] = (uint32_t)(lba & 0xFFFFFFFFu);
        s[11] = (uint32_t)(lba >> 32);
        s[12] = (blocks - 1u) & 0xFFFFu;

        if (nvme_fill_prp(s, p, chunk_bytes) == 0) {
            if (nvme_io(s) != 0) return -1;
        } else {
            /* Hizasiz veya haritasiz tampon: 64 KiB bounce yedegi */
            uint32_t b_max = NVME_BOUNCE_SZ / sec_bytes;
            if (b_max == 0) b_max = 1;
            if (blocks > b_max) {
                blocks = b_max;
                chunk_bytes = blocks * sec_bytes;
                s[12] = (blocks - 1u) & 0xFFFFu;
            }
            if (nvme_fill_prp(s, s_bounce, chunk_bytes) != 0) return -1;
            if (write) memcpy(s_bounce, p, chunk_bytes);
            if (nvme_io(s) != 0) return -1;
            if (!write) memcpy(p, s_bounce, chunk_bytes);
        }

        p += chunk_bytes;
        lba512 += chunk_bytes / 512u;
        count512 -= chunk_bytes / 512u;
    }
    return 0;
}

static int nvme_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    (void)ctx; return nvme_transfer(lba, count, buf, 0);
}
static int nvme_write(void *ctx, uint64_t lba, uint32_t count, const void *buf) {
    (void)ctx; return nvme_transfer(lba, count, (void *)buf, 1);
}
/* NVM I/O Flush (opcode 0x00, I/O kuyrugu qid=1). */
static int nvme_flush(void *ctx) {
    (void)ctx;
    if (!g_ready) return -1;
    uint32_t s[16];
    g_io_cid = (uint16_t)((g_io_cid + 1u) & 0xFFFFu);
    nvme_mk_sqe(s, 0x00, g_io_cid);
    s[1] = g_nsid;
    return nvme_io(s);
}

static void nvme_register(void) {
    bdev_ops_t ops;
    for (int i = 0; i < 24; i++) ops.name[i] = 0;
    const char *nm = "nvme0";
    for (int i = 0; nm[i]; i++) ops.name[i] = nm[i];
    ops.kind = BDEV_KIND_NVME;
    ops.sectors = g_nsze << (g_lba_shift - 9);
    ops.removable = 0;
    ops.ctx = 0;
    ops.read = nvme_read;
    ops.write = nvme_write;
    ops.flush = nvme_flush;
    blockdev_register(&ops);
}

#ifdef ARCTIAN_BLK_SELFTEST
static void nvme_selftest_rw(void) {
    uint64_t total_secs = g_nsze << (g_lba_shift - 9);
    if (total_secs < 1024u) return;

    /* Tahribatsiz PRP1, PRP1+PRP2 (8 KiB), ve PRP-List (128 KiB = 32 sayfa) dogrulamasi */
    uint64_t test_lba = total_secs - 512u;
    const uint32_t nsecs = 256u;                 /* 128 KiB = 32 sayfa */
    const uint32_t nbytes = nsecs * 512u;
    uint8_t *save = (uint8_t *)nvme_alloc_page(nbytes);
    uint8_t *wbuf = (uint8_t *)nvme_alloc_page(nbytes);
    uint8_t *rbuf = (uint8_t *)nvme_alloc_page(nbytes);
    if (!save || !wbuf || !rbuf) {
        NDBG("[nvme] selftest alloc fail\n");
        return;
    }

    if (nvme_transfer(test_lba, nsecs, save, 0) != 0) {
        NDBG("[nvme] selftest save-read fail\n");
        return;
    }

    /* 1) 1-sayfa PRP1 (4 KiB), 2-sayfa PRP1+PRP2 (8 KiB), 32-sayfa PRP-list (128 KiB) */
    for (uint32_t i = 0; i < nbytes; i++)
        wbuf[i] = (uint8_t)((i * 73u + 0x3Cu) ^ (i >> 8));

    int w4 = nvme_transfer(test_lba, 8u, wbuf, 1);
    int f4 = nvme_flush(0);
    int r4 = nvme_transfer(test_lba, 8u, rbuf, 0);
    int m4 = memcmp(wbuf, rbuf, 4096u);
    int ok_4k = (w4 == 0 && f4 == 0 && r4 == 0 && m4 == 0);
    if (!ok_4k) NDBG("[nvme] 4k detail: w=%d f=%d r=%d m=%d rbuf0=%02X wbuf0=%02X\n",
                     w4, f4, r4, m4, rbuf[0], wbuf[0]);

    memset(rbuf, 0, 8192u);
    int ok_8k = (nvme_transfer(test_lba, 16u, wbuf, 1) == 0 &&
                 nvme_flush(0) == 0 &&
                 nvme_transfer(test_lba, 16u, rbuf, 0) == 0 &&
                 memcmp(wbuf, rbuf, 8192u) == 0);

    memset(rbuf, 0, nbytes);
    int ok_128k = (nvme_transfer(test_lba, nsecs, wbuf, 1) == 0 &&
                   nvme_flush(0) == 0 &&
                   nvme_transfer(test_lba, nsecs, rbuf, 0) == 0 &&
                   memcmp(wbuf, rbuf, nbytes) == 0);

    NDBG("[nvme] prp1(4KB)=%s prp1+2(8KB)=%s prp-list(128KB)=%s\n",
         ok_4k ? "OK" : "FAIL", ok_8k ? "OK" : "FAIL", ok_128k ? "OK" : "FAIL");

    (void)nvme_transfer(test_lba, nsecs, save, 1);
    (void)nvme_flush(0);
}
#endif

void blk_nvme_scan(void) {
    if (g_ready) { nvme_register(); return; }
    if (nvme_ensure_mem() != 0) return;

    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint32_t id = blk_pci_readl((uint8_t)bus, (uint8_t)slot, 0, 0x08);
            uint8_t cc = (uint8_t)(id >> 24), sc = (uint8_t)(id >> 16), pi = (uint8_t)(id >> 8);
            if (cc != 0x01 || sc != 0x08 || pi != 0x02) continue;

            blk_pci_enable((uint8_t)bus, (uint8_t)slot, 0);
            uint64_t bar = blk_pci_bar((uint8_t)bus, (uint8_t)slot, 0, 0) & ~0xFULL;
            NDBG("[nvme] bus=%d slot=%d bar=%llX\n", bus, slot, (unsigned long long)bar);
            if (!bar) continue;
            blk_mmio_map(bar, 0x4000);
            g_mmio = (volatile uint32_t *)(uintptr_t)bar;

            uint64_t cap = (uint64_t)nvme_rd(NVME_REG_CAP) |
                           ((uint64_t)nvme_rd(NVME_REG_CAP + 4) << 32);
            uint32_t dstrd = (uint32_t)((cap >> 32) & 0xF);
            uint32_t mpsmin = (uint32_t)((cap >> 48) & 0xF);
            g_stride = 4u << dstrd;
            NDBG("[nvme] cap=%llX dstrd=%d mpsmin=%u cst=%08X cc=%08X\n",
                 (unsigned long long)cap, dstrd, mpsmin,
                 nvme_rd(NVME_REG_CST), nvme_rd(NVME_REG_CC));

            nvme_wr(NVME_REG_INTMS, 0xFFFFFFFFu);
            nvme_wr(NVME_REG_CC, 0);
            if (nvme_wait_ready(0) != 0) { NDBG("[nvme] disable-timeout cst=%08X\n", nvme_rd(NVME_REG_CST)); continue; }
            NDBG("[nvme] disabled ok\n");

            uint64_t asq_pa = blk_virt_to_phys(s_asq);
            uint64_t acq_pa = blk_virt_to_phys(s_acq);
            if (!asq_pa || !acq_pa) continue;

            nvme_wr(NVME_REG_AQA, (NVME_AQ_ENTRIES - 1) | ((NVME_AQ_ENTRIES - 1) << 16));
            nvme_wr(NVME_REG_ASQ, (uint32_t)(asq_pa & 0xFFFFFFFFu));
            nvme_wr(NVME_REG_ASQ + 4, (uint32_t)(asq_pa >> 32));
            nvme_wr(NVME_REG_ACQ, (uint32_t)(acq_pa & 0xFFFFFFFFu));
            nvme_wr(NVME_REG_ACQ + 4, (uint32_t)(acq_pa >> 32));

            g_asq_tail = 0; g_acq_head = 0; g_phase = 1;
            g_sq_tail = 0; g_cq_head = 0; g_io_phase = 1;

            uint32_t ccfg = (6u << 16) | (4u << 20) | 1u;  /* IOSQES=6, IOCQES=4, EN */
            nvme_wr(NVME_REG_CC, ccfg);
            if (nvme_wait_ready(1) != 0) { NDBG("[nvme] enable-timeout cst=%08X\n", nvme_rd(NVME_REG_CST)); continue; }
            nvme_wr(NVME_REG_INTMS, 0xFFFFFFFFu);
            NDBG("[nvme] enabled ok\n");

            /* Identify Controller (CNS = 1) ile MDTS oku */
            uint32_t s[16];
            uint64_t id_pa = blk_virt_to_phys(s_ident);
            if (!id_pa) continue;
            nvme_mk_sqe(s, 0x06, 1);
            s[1] = 0;
            s[6] = (uint32_t)(id_pa & 0xFFFFFFFFu);
            s[7] = (uint32_t)(id_pa >> 32);
            s[10] = 1;                   /* CNS = 1: controller */
            if (nvme_admin(s, 0) == 0) {
                uint8_t mdts = s_ident[77];
                if (mdts != 0 && mdts < 20) {
                    uint32_t mdts_bytes = (1u << mdts) * (1u << (12u + mpsmin));
                    if (mdts_bytes >= NVME_PAGE && mdts_bytes < g_max_xfer)
                        g_max_xfer = mdts_bytes;
                }
                NDBG("[nvme] mdts=%u max_xfer=%u\n", (unsigned)mdts, (unsigned)g_max_xfer);
            }

            /* Identify Namespace 1 (CNS = 0) */
            nvme_mk_sqe(s, 0x06, 2);
            s[1] = 1;                    /* NSID = 1 */
            s[6] = (uint32_t)(id_pa & 0xFFFFFFFFu);
            s[7] = (uint32_t)(id_pa >> 32);
            s[10] = 0;                   /* CNS = 0: namespace */
            if (nvme_admin(s, 0) != 0) { NDBG("[nvme] identify-ns fail\n"); continue; }

            uint64_t nsze;
            memcpy(&nsze, s_ident + 0, 8);
            uint8_t flbas = s_ident[26];
            uint8_t lbaf_idx = flbas & 0xF;
            uint8_t lbads = s_ident[128 + lbaf_idx * 4 + 2];
            if (lbads >= 9 && lbads <= 12) g_lba_shift = lbads;
            else g_lba_shift = 9;
            g_nsze = nsze;
            g_nsid = 1;
            NDBG("[nvme] nsze=%u lbads=%d\n", (unsigned)nsze, (int)lbads);

            /* I/O Completion Queue olustur (qid=1) */
            uint64_t iocq_pa = blk_virt_to_phys(s_iocq);
            uint64_t iosq_pa = blk_virt_to_phys(s_iosq);
            if (!iocq_pa || !iosq_pa) continue;

            nvme_mk_sqe(s, 0x05, 3);
            s[6] = (uint32_t)(iocq_pa & 0xFFFFFFFFu);
            s[7] = (uint32_t)(iocq_pa >> 32);
            s[10] = 1 | ((NVME_IO_ENTRIES - 1) << 16);
            s[11] = 1;   /* PC=1 */
            { int r1 = nvme_admin(s, 0); NDBG("[nvme] create-cq r=%d\n", r1); if (r1) continue; }

            /* I/O Submission Queue olustur (qid=1, cqid=1) */
            nvme_mk_sqe(s, 0x01, 4);
            s[6] = (uint32_t)(iosq_pa & 0xFFFFFFFFu);
            s[7] = (uint32_t)(iosq_pa >> 32);
            s[10] = 1 | ((NVME_IO_ENTRIES - 1) << 16);
            s[11] = 1 | (1u << 16);  /* PC=1 | CQID=1 */
            { int r2 = nvme_admin(s, 0); NDBG("[nvme] create-sq r=%d\n", r2); if (r2) continue; }

            g_ready = 1;
#ifdef ARCTIAN_BLK_SELFTEST
            nvme_selftest_rw();
#endif
            nvme_register();
            return;
        }
    }
}
