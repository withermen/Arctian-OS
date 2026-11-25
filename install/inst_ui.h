#ifndef ARCTIAN_INST_UI_H
#define ARCTIAN_INST_UI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "kernel.h"
#include "bootinfo.h"
#include "blockdev.h"
#include "font8x8.h"

/* =========================================================================
 * Arctian OS Release Graphical Installer UI (Zero-alloc, Flicker-Free)
 * ========================================================================= */

#define INST_MAX_DISKS      BLOCKDEV_MAX
#define INST_MAX_VISIBLE    5
#define INST_MAX_CARD_W     760
#define INST_STRIP_H        64

/* Theme Palette (Arctian Dark Slate) */
#define COL_BG_TOP          0x0B101Bu
#define COL_BG_BOT          0x111A2Eu
#define COL_CARD_BG         0x141E30u
#define COL_CARD_HDR        0x19253Au
#define COL_CARD_BORDER     0x26354Fu
#define COL_SHADOW          0x070B13u
#define COL_ACCENT          0x38BDF8u
#define COL_ACCENT_DARK     0x0284C7u
#define COL_ACCENT_BG       0x1A2F4Bu
#define COL_ROW_BG          0x182336u
#define COL_ROW_BORDER      0x27364Fu
#define COL_TEXT_PRI        0xF8FAFCu
#define COL_TEXT_SEC        0x94A3B8u
#define COL_TEXT_DIM        0x64748Bu
#define COL_OK              0x22C55Eu
#define COL_OK_BG           0x132E24u
#define COL_WARN            0xF59E0Bu
#define COL_WARN_BG         0x332412u
#define COL_ERR             0xEF4444u
#define COL_ERR_BG          0x35171Du
#define COL_NVME            0x818CF8u
#define COL_SATA            0x38BDF8u
#define COL_ATA             0xFBBF24u
#define COL_USB             0x34D399u
#define COL_TRACK           0x0D1524u

/* Key codes returned by inst_poll_key() */
typedef enum {
    INST_KEY_NONE = 0,
    INST_KEY_UP,
    INST_KEY_DOWN,
    INST_KEY_ENTER,
    INST_KEY_ESC,
    INST_KEY_RESCAN,
    INST_KEY_1,
    INST_KEY_2,
    INST_KEY_3,
    INST_KEY_4,
    INST_KEY_5,
    INST_KEY_6,
    INST_KEY_7,
    INST_KEY_8,
    INST_KEY_9
} inst_key_t;

typedef struct {
    int             bdev_idx;
    blockdev_info_t info;
    uint64_t        size_mb;
    uint8_t         has_arctian;
    uint8_t         too_small;
    bool            has_gpt;
    bool            has_efi_part;
    bool            has_mbr;
    uint32_t        gpt_parts;
} inst_disk_t;

typedef struct {
    volatile uint8_t *addr;
    uint64_t          phys;
    uint32_t          width;
    uint32_t          height;
    uint32_t          pitch;
    uint8_t           bpp;
    uint8_t           is_rgb;
    uint8_t           active;
} inst_fb_t;

typedef struct {
    int card_x;
    int card_y;
    int card_w;
    int card_h;
    int hdr_h;
    int body_y;
    int ftr_y;
} inst_layout_t;

/* Dedicated page-table storage so framebuffer mapping never collides with
 * mmio_reset() inside blockdev_init(). */
static uint64_t s_fb_pdpt[512] __attribute__((aligned(4096)));
static uint64_t s_fb_pd[4][512] __attribute__((aligned(4096)));
static int      s_fb_pd_used = 0;

/* Offscreen composition buffers (kept small to fit comfortably < 1 MB total). */
static uint32_t s_strip[INST_MAX_CARD_W * INST_STRIP_H];
static uint32_t s_scanline[3840];

static inst_fb_t     g_fb;
static inst_layout_t g_lay;
static bool          s_e0_prefix = false;

/* -------------------------------------------------------------------------
 * Low-level helpers (color, string, time)
 * ------------------------------------------------------------------------- */
static uint32_t inst_lerp_color(uint32_t c0, uint32_t c1, int num, int den) {
    if (den <= 0 || num <= 0) return c0;
    if (num >= den) return c1;
    uint32_t r0 = (c0 >> 16) & 0xFFu, g0 = (c0 >> 8) & 0xFFu, b0 = c0 & 0xFFu;
    uint32_t r1 = (c1 >> 16) & 0xFFu, g1 = (c1 >> 8) & 0xFFu, b1 = c1 & 0xFFu;
    uint32_t r = (r0 * (uint32_t)(den - num) + r1 * (uint32_t)num) / (uint32_t)den;
    uint32_t g = (g0 * (uint32_t)(den - num) + g1 * (uint32_t)num) / (uint32_t)den;
    uint32_t b = (b0 * (uint32_t)(den - num) + b1 * (uint32_t)num) / (uint32_t)den;
    return (r << 16) | (g << 8) | b;
}

static char *inst_str_append(char *dst, char *end, const char *src) {
    while (*src && dst + 1 < end) *dst++ = *src++;
    *dst = '\0';
    return dst;
}

static char *inst_u64_append(char *dst, char *end, uint64_t v) {
    char tmp[24];
    int n = 0;
    if (v == 0) {
        if (dst + 1 < end) *dst++ = '0';
        *dst = '\0';
        return dst;
    }
    while (v && n < 23) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0 && dst + 1 < end) *dst++ = tmp[--n];
    *dst = '\0';
    return dst;
}

static void inst_fmt_bytes(uint64_t bytes, char *out, size_t out_sz) {
    char *p = out;
    char *end = out + out_sz;
    const uint64_t mib = 1024ULL * 1024ULL;
    const uint64_t gib = 1024ULL * mib;
    const uint64_t tib = 1024ULL * gib;
    if (bytes >= tib) {
        uint64_t mibs = (bytes + (mib / 2ULL)) / mib;
        uint64_t tenths = (mibs * 10ULL + 512ULL * 1024ULL) / (1024ULL * 1024ULL);
        p = inst_u64_append(p, end, tenths / 10ULL);
        p = inst_str_append(p, end, ".");
        p = inst_u64_append(p, end, tenths % 10ULL);
        inst_str_append(p, end, " TB");
    } else if (bytes >= gib) {
        uint64_t tenths = (bytes * 10ULL + (gib / 2ULL)) / gib;
        p = inst_u64_append(p, end, tenths / 10ULL);
        p = inst_str_append(p, end, ".");
        p = inst_u64_append(p, end, tenths % 10ULL);
        inst_str_append(p, end, " GB");
    } else {
        uint64_t tenths = (bytes * 10ULL + (mib / 2ULL)) / mib;
        p = inst_u64_append(p, end, tenths / 10ULL);
        p = inst_str_append(p, end, ".");
        p = inst_u64_append(p, end, tenths % 10ULL);
        inst_str_append(p, end, " MB");
    }
}

static void inst_fmt_size(uint64_t sectors, char *out, size_t out_sz) {
    if (sectors <= (~0ULL / 512ULL)) {
        inst_fmt_bytes(sectors * 512ULL, out, out_sz);
        return;
    }
    char *p = out;
    char *end = out + out_sz;
    uint64_t mb = sectors / 2048ULL;
    if (mb >= 1024ULL * 1024ULL) {
        uint64_t tb_tenths = (mb * 10ULL + (512ULL * 1024ULL)) / (1024ULL * 1024ULL);
        p = inst_u64_append(p, end, tb_tenths / 10ULL);
        p = inst_str_append(p, end, ".");
        p = inst_u64_append(p, end, tb_tenths % 10ULL);
        inst_str_append(p, end, " TB");
    } else if (mb >= 1024ULL) {
        uint64_t gb_tenths = (mb * 10ULL + 512ULL) / 1024ULL;
        p = inst_u64_append(p, end, gb_tenths / 10ULL);
        p = inst_str_append(p, end, ".");
        p = inst_u64_append(p, end, gb_tenths % 10ULL);
        inst_str_append(p, end, " GB");
    } else {
        p = inst_u64_append(p, end, mb);
        inst_str_append(p, end, " MB");
    }
}

static uint8_t inst_cmos_sec(void) {
    /* Wait briefly if RTC update-in-progress bit is set */
    for (int i = 0; i < 10000; i++) {
        outb(0x70, 0x0Au);
        if (!(inb(0x71) & 0x80u)) break;
    }
    outb(0x70, 0x00u);
    return inb(0x71);
}

/* -------------------------------------------------------------------------
 * Framebuffer Discovery, Page Mapping, and Pixel Blit
 * ------------------------------------------------------------------------- */
static void inst_map_fb(uint64_t phys, uint64_t size) {
    if (size == 0) size = 8ULL * 1024ULL * 1024ULL;
    uint64_t start = phys & ~0x1FFFFFULL;
    uint64_t end = (phys + size + 0x1FFFFFULL) & ~0x1FFFFFULL;
    if (end <= 0x40000000ULL) return; /* <1GB already identity-mapped by stage2 */

    uint64_t cr3 = 0;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t *pml4 = (uint64_t *)(uintptr_t)(cr3 & ~0xFFFULL);
    int i4 = (int)((start >> 39) & 0x1FFu);

    uint64_t *pdpt;
    if (!(pml4[i4] & 1ULL)) {
        for (int i = 0; i < 512; i++) s_fb_pdpt[i] = 0;
        pdpt = s_fb_pdpt;
        pml4[i4] = (uint64_t)(uintptr_t)pdpt | 0x3ULL;
    } else {
        pdpt = (uint64_t *)(uintptr_t)(pml4[i4] & ~0xFFFULL);
    }

    for (uint64_t a = start; a < end; a += 0x40000000ULL) {
        int i3 = (int)((a >> 30) & 0x1FFu);
        if (pdpt[i3] & 0x80ULL) continue; /* already 1GB huge page */
        uint64_t *pd;
        if (pdpt[i3] & 1ULL) {
            pd = (uint64_t *)(uintptr_t)(pdpt[i3] & ~0xFFFULL);
        } else {
            if (s_fb_pd_used >= 4) break;
            pd = s_fb_pd[s_fb_pd_used++];
            for (int k = 0; k < 512; k++) pd[k] = 0;
            pdpt[i3] = (uint64_t)(uintptr_t)pd | 0x3ULL;
        }
        uint64_t seg_end = a + 0x40000000ULL;
        if (seg_end > end) seg_end = end;
        for (uint64_t b = a; b < seg_end; b += 0x200000ULL) {
            int i2 = (int)((b >> 21) & 0x1FFu);
            /* 0x8B = Present | Write | PWT (PAT WC if enabled) | 2MB Huge */
            pd[i2] = b | 0x8BULL;
        }
    }
    asm volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

static uint32_t inst_pci_readl(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) |
                    ((uint32_t)slot << 11) | ((uint32_t)func << 8) | (off & 0xFCu);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

static uint64_t inst_find_bga_lfb(void) {
    for (int bus = 0; bus < 8; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint32_t vid = inst_pci_readl((uint8_t)bus, (uint8_t)slot, 0, 0x00);
            if ((vid & 0xFFFFu) == 0xFFFFu) continue;
            uint32_t cls = inst_pci_readl((uint8_t)bus, (uint8_t)slot, 0, 0x08);
            if (((cls >> 24) & 0xFFu) == 0x03u) {
                uint32_t bar0 = inst_pci_readl((uint8_t)bus, (uint8_t)slot, 0, 0x10);
                if (!(bar0 & 1u) && (bar0 & ~0xFu)) {
                    return (uint64_t)(bar0 & ~0xFu);
                }
            }
        }
    }
    return 0xFD000000ULL;
}

static bool inst_fb_init(void) {
    g_fb.active = 0;
    const arctian_bootinfo_t *bi = arctian_bootinfo();

    if (arctian_bootinfo_valid(bi) && bi->width >= 640u && bi->height >= 480u) {
        uint64_t fb_phys = (uint64_t)bi->fb_phys;
        uint64_t sz = bi->fb_size ? (uint64_t)bi->fb_size : 0ULL;
        if (arctian_bootinfo_is_uefi(bi)) {
            const volatile uint32_t *ui32 = (const volatile uint32_t *)(uintptr_t)0x7100u;
            if (ui32[0] == 0x49464555u && ui32[1] >= 0x60u) {
                uint64_t uefi_fb = *(const volatile uint64_t *)(uintptr_t)(0x7100u + 0x50u);
                uint64_t uefi_sz = *(const volatile uint64_t *)(uintptr_t)(0x7100u + 0x58u);
                if (uefi_fb != 0) fb_phys = uefi_fb;
                if (uefi_sz != 0) sz = uefi_sz;
            }
        }
        if (fb_phys != 0 && fb_phys != 0xFFFFFFFFULL) {
            g_fb.phys   = fb_phys;
            g_fb.width  = bi->width;
            g_fb.height = bi->height;
            g_fb.bpp    = bi->bpp;
            g_fb.pitch  = bi->pitch ? bi->pitch : (uint32_t)bi->width * (uint32_t)(bi->bpp / 8u);
            g_fb.is_rgb = (bi->format == ARCTIAN_PIXFMT_RGBX8 ||
                           bi->format == ARCTIAN_PIXFMT_RGBA8) ? 1 : 0;
            if (sz == 0) sz = (uint64_t)g_fb.pitch * (uint64_t)g_fb.height;
            inst_map_fb(g_fb.phys, sz);
            g_fb.addr = (volatile uint8_t *)(uintptr_t)g_fb.phys;
            g_fb.active = 1;
        }
    } else {
        /* Fallback: probe Bochs BGA (0x01CE / 0x01CF) */
        outw(0x01CE, 0);
        outw(0x01CF, 0xB0C5);
        outw(0x01CE, 0);
        uint16_t id = inw(0x01CF);
        if (id >= 0xB0C0 && id <= 0xB0C5) {
            outw(0x01CE, 4); outw(0x01CF, 0x00);
            outw(0x01CE, 1); outw(0x01CF, 1024);
            outw(0x01CE, 2); outw(0x01CF, 768);
            outw(0x01CE, 3); outw(0x01CF, 32);
            outw(0x01CE, 6); outw(0x01CF, 1024);
            outw(0x01CE, 4); outw(0x01CF, 0x41);
            outw(0x01CE, 1);
            if (inw(0x01CF) == 1024) {
                g_fb.phys   = inst_find_bga_lfb();
                g_fb.width  = 1024;
                g_fb.height = 768;
                g_fb.bpp    = 32;
                g_fb.pitch  = 1024u * 4u;
                g_fb.is_rgb = 0;
                inst_map_fb(g_fb.phys, (uint64_t)g_fb.pitch * g_fb.height);
                g_fb.addr = (volatile uint8_t *)(uintptr_t)g_fb.phys;
                g_fb.active = 1;
            }
        }
    }

    if (!g_fb.active) return false;

    int cw = (int)g_fb.width - 64;
    if (cw > INST_MAX_CARD_W) cw = INST_MAX_CARD_W;
    if (cw < 560) cw = (int)g_fb.width - 16;

    int ch = 468;
    if ((int)g_fb.height - 64 < ch) ch = (int)g_fb.height - 32;

    g_lay.card_w = cw;
    g_lay.card_h = ch;
    g_lay.card_x = ((int)g_fb.width - cw) / 2;
    g_lay.card_y = ((int)g_fb.height - ch) / 2;
    g_lay.hdr_h  = 72;
    g_lay.body_y = g_lay.card_y + g_lay.hdr_h + 14;
    g_lay.ftr_y  = g_lay.card_y + ch - 40;
    return true;
}

static inline uint32_t inst_pack_pixel(uint32_t rgb) {
    if (!g_fb.is_rgb) return rgb;
    return ((rgb & 0xFFu) << 16) | (rgb & 0x00FF00u) | ((rgb >> 16) & 0xFFu);
}

static void inst_blit_rect(int x, int y, int w, int h, const uint32_t *src, int src_stride) {
    if (!g_fb.active || w <= 0 || h <= 0) return;
    if (x < 0) { src -= x; w += x; x = 0; }
    if (y < 0) { src -= y * src_stride; h += y; y = 0; }
    if (x + w > (int)g_fb.width)  w = (int)g_fb.width - x;
    if (y + h > (int)g_fb.height) h = (int)g_fb.height - y;
    if (w <= 0 || h <= 0) return;

    if (g_fb.bpp == 32 && !g_fb.is_rgb) {
        for (int r = 0; r < h; r++) {
            volatile uint32_t *dst = (volatile uint32_t *)(g_fb.addr + (size_t)(y + r) * g_fb.pitch) + x;
            const uint32_t *srow = src + (size_t)r * (size_t)src_stride;
            for (int c = 0; c < w; c++) dst[c] = srow[c];
        }
    } else if (g_fb.bpp == 32) {
        for (int r = 0; r < h; r++) {
            volatile uint32_t *dst = (volatile uint32_t *)(g_fb.addr + (size_t)(y + r) * g_fb.pitch) + x;
            const uint32_t *srow = src + (size_t)r * (size_t)src_stride;
            for (int c = 0; c < w; c++) dst[c] = inst_pack_pixel(srow[c]);
        }
    } else if (g_fb.bpp == 24) {
        for (int r = 0; r < h; r++) {
            volatile uint8_t *dst = g_fb.addr + (size_t)(y + r) * g_fb.pitch + (size_t)x * 3u;
            const uint32_t *srow = src + (size_t)r * (size_t)src_stride;
            for (int c = 0; c < w; c++) {
                uint32_t px = srow[c];
                if (g_fb.is_rgb) {
                    dst[c * 3 + 0] = (uint8_t)(px >> 16);
                    dst[c * 3 + 1] = (uint8_t)(px >> 8);
                    dst[c * 3 + 2] = (uint8_t)(px);
                } else {
                    dst[c * 3 + 0] = (uint8_t)(px);
                    dst[c * 3 + 1] = (uint8_t)(px >> 8);
                    dst[c * 3 + 2] = (uint8_t)(px >> 16);
                }
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * Offscreen Surface Drawing Primitives (inside s_strip[w * h])
 * ------------------------------------------------------------------------- */
static void surf_fill(uint32_t *buf, int stride, int w, int h, uint32_t col) {
    for (int y = 0; y < h; y++) {
        uint32_t *row = buf + y * stride;
        for (int x = 0; x < w; x++) row[x] = col;
    }
}

static void surf_fill_rect(uint32_t *buf, int stride, int bw, int bh,
                           int x, int y, int w, int h, uint32_t col) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > bw) w = bw - x;
    if (y + h > bh) h = bh - y;
    if (w <= 0 || h <= 0) return;
    for (int r = 0; r < h; r++) {
        uint32_t *dst = buf + (y + r) * stride + x;
        for (int c = 0; c < w; c++) dst[c] = col;
    }
}

static bool pt_in_rrect(int px, int py, int w, int h, int rad) {
    if (px < 0 || py < 0 || px >= w || py >= h) return false;
    if (rad <= 0) return true;
    int cx = -1, cy = -1;
    if (px < rad && py < rad) { cx = rad - 1; cy = rad - 1; }
    else if (px >= w - rad && py < rad) { cx = w - rad; cy = rad - 1; }
    else if (px < rad && py >= h - rad) { cx = rad - 1; cy = h - rad; }
    else if (px >= w - rad && py >= h - rad) { cx = w - rad; cy = h - rad; }
    if (cx < 0) return true;
    int dx = px - cx, dy = py - cy;
    return (dx * dx + dy * dy) <= (rad * rad);
}

static void surf_round_rect(uint32_t *buf, int stride, int bw, int bh,
                            int x, int y, int w, int h, int rad,
                            uint32_t fill, uint32_t border) {
    for (int py = 0; py < h; py++) {
        int dy = y + py;
        if (dy < 0 || dy >= bh) continue;
        for (int px = 0; px < w; px++) {
            int dx = x + px;
            if (dx < 0 || dx >= bw) continue;
            if (!pt_in_rrect(px, py, w, h, rad)) continue;
            bool is_edge = !pt_in_rrect(px - 1, py, w, h, rad) ||
                           !pt_in_rrect(px + 1, py, w, h, rad) ||
                           !pt_in_rrect(px, py - 1, w, h, rad) ||
                           !pt_in_rrect(px, py + 1, w, h, rad);
            buf[dy * stride + dx] = is_edge ? border : fill;
        }
    }
}

/* Crisp bitmap text rendering:
 * scale=1 -> 8x16 crisp (each 8x8 row doubled vertically for readable 8x16 UI text)
 * scale=2 -> 16x16 Scale2x-smoothed bold title/percentage text */
static void surf_draw_char(uint32_t *buf, int stride, int bw, int bh,
                           int x, int y, char c, uint32_t fg, int scale) {
    int idx = (unsigned char)c - 32;
    if (idx < 0 || idx >= 95) return;

    if (scale >= 2) {
        /* 2x scale (16x16) with Scale2x smoothing for crisp large headers */
        for (int row = 0; row < 8; row++) {
            uint8_t cur = font8x8_basic[idx][row];
            uint8_t up  = (row > 0) ? font8x8_basic[idx][row - 1] : 0;
            uint8_t dn  = (row < 7) ? font8x8_basic[idx][row + 1] : 0;
            for (int col = 0; col < 8; col++) {
                uint8_t mask = (uint8_t)(0x80u >> col);
                int p = (cur & mask) != 0;
                int a = (up & mask) != 0;
                int d = (dn & mask) != 0;
                int l = (col > 0 && (cur & (0x80u >> (col - 1)))) ? 1 : 0;
                int r = (col < 7 && (cur & (0x80u >> (col + 1)))) ? 1 : 0;

                int e0 = (l == a && l != d && a != r) ? a : p;
                int e1 = (a == r && a != l && r != d) ? r : p;
                int e2 = (l == d && l != a && d != r) ? l : p;
                int e3 = (d == r && l != d && a != r) ? d : p;

                int sx = x + col * 2;
                int sy = y + row * 2;
                if (e0 && sx >= 0 && sx < bw && sy >= 0 && sy < bh) buf[sy * stride + sx] = fg;
                if (e1 && sx + 1 >= 0 && sx + 1 < bw && sy >= 0 && sy < bh) buf[sy * stride + sx + 1] = fg;
                if (e2 && sx >= 0 && sx < bw && sy + 1 >= 0 && sy + 1 < bh) buf[(sy + 1) * stride + sx] = fg;
                if (e3 && sx + 1 >= 0 && sx + 1 < bw && sy + 1 >= 0 && sy + 1 < bh) buf[(sy + 1) * stride + sx + 1] = fg;
            }
        }
    } else {
        /* 8x16 vertical-doubled crisp body text (9px advance) */
        for (int row = 0; row < 8; row++) {
            uint8_t bits = font8x8_basic[idx][row];
            int dy0 = y + row * 2;
            int dy1 = dy0 + 1;
            for (int col = 0; col < 8; col++) {
                if (bits & (0x80u >> col)) {
                    int dx = x + col;
                    if (dx < 0 || dx >= bw) continue;
                    if (dy0 >= 0 && dy0 < bh) buf[dy0 * stride + dx] = fg;
                    if (dy1 >= 0 && dy1 < bh) buf[dy1 * stride + dx] = fg;
                }
            }
        }
    }
}

static int inst_text_width(const char *s, int scale) {
    int n = 0;
    while (s[n]) n++;
    return n * (scale >= 2 ? 16 : 9);
}

static void surf_draw_text(uint32_t *buf, int stride, int bw, int bh,
                           int x, int y, const char *s, uint32_t fg, int scale) {
    int adv = (scale >= 2) ? 16 : 9;
    while (*s) {
        surf_draw_char(buf, stride, bw, bh, x, y, *s, fg, scale);
        x += adv;
        s++;
    }
}

/* Draw a pill badge and return its total pixel width */
static int surf_draw_badge(uint32_t *buf, int stride, int bw, int bh,
                           int x, int y, const char *label,
                           uint32_t fg, uint32_t bg, uint32_t border) {
    int tw = inst_text_width(label, 1);
    int pw = tw + 16;
    int ph = 24;
    surf_round_rect(buf, stride, bw, bh, x, y, pw, ph, 6, bg, border);
    surf_draw_text(buf, stride, bw, bh, x + 8, y + 4, label, fg, 1);
    return pw;
}

/* -------------------------------------------------------------------------
 * Full Frame Background + Card Shell + Header (drawn on screen transitions)
 * ------------------------------------------------------------------------- */
static void inst_draw_shell(const char *subtitle) {
    if (!g_fb.active) return;

    int fw = (int)g_fb.width;
    int fh = (int)g_fb.height;
    int line_w = fw < 3840 ? fw : 3840;
    int cx = g_lay.card_x, cy = g_lay.card_y;
    int cw = g_lay.card_w, ch = g_lay.card_h;

    for (int y = 0; y < fh; y++) {
        uint32_t bg;
        if (y < 3) {
            bg = COL_ACCENT;
        } else {
            bg = inst_lerp_color(COL_BG_TOP, COL_BG_BOT, y, fh);
        }

        for (int x = 0; x < line_w; x++) s_scanline[x] = bg;

        /* Soft rounded drop shadow behind card */
        if (y >= cy + 6 && y < cy + ch + 6) {
            int sy = y - (cy + 6);
            int sx0 = cx + 6, sx1 = cx + cw + 6;
            if (sx0 < 0) sx0 = 0;
            if (sx1 > line_w) sx1 = line_w;
            for (int x = sx0; x < sx1; x++) {
                if (pt_in_rrect(x - (cx + 6), sy, cw, ch, 12)) {
                    s_scanline[x] = COL_SHADOW;
                }
            }
        }

        /* Card frame */
        if (y >= cy && y < cy + ch) {
            int ly = y - cy;
            uint32_t fill = (ly < g_lay.hdr_h) ? COL_CARD_HDR : COL_CARD_BG;
            for (int px = 0; px < cw; px++) {
                if (!pt_in_rrect(px, ly, cw, ch, 12)) continue;
                bool edge = !pt_in_rrect(px - 1, ly, cw, ch, 12) ||
                            !pt_in_rrect(px + 1, ly, cw, ch, 12) ||
                            !pt_in_rrect(px, ly - 1, cw, ch, 12) ||
                            !pt_in_rrect(px, ly + 1, cw, ch, 12);
                uint32_t col = edge ? COL_CARD_BORDER : fill;
                if (ly == g_lay.hdr_h) col = COL_CARD_BORDER;
                if (ly >= 1 && ly <= 2 && !edge && px >= 12 && px < cw - 12) col = COL_ACCENT;
                if (cx + px >= 0 && cx + px < line_w) s_scanline[cx + px] = col;
            }
        }

        inst_blit_rect(0, y, line_w, 1, s_scanline, line_w);
    }

    /* Header strip inside card (flicker-free in s_strip) */
    int hdr_strip_h = 62;
    surf_fill(s_strip, cw, cw, hdr_strip_h, COL_CARD_HDR);
    /* Geometric Arctian emblem badge on top-left */
    surf_round_rect(s_strip, cw, cw, hdr_strip_h, 24, 12, 38, 38, 8, COL_ACCENT_BG, COL_ACCENT);
    surf_draw_text(s_strip, cw, cw, hdr_strip_h, 35, 23, "A", COL_ACCENT, 2);

    surf_draw_text(s_strip, cw, cw, hdr_strip_h, 76, 12, "ARCTIAN OS KURULUM", COL_TEXT_PRI, 2);
    surf_draw_text(s_strip, cw, cw, hdr_strip_h, 76, 36, subtitle, COL_TEXT_SEC, 1);

    /* Version / Live pill on top-right */
    const char *tag = "CANLI ISO";
    int tw = inst_text_width(tag, 1) + 18;
    surf_draw_badge(s_strip, cw, cw, hdr_strip_h, cw - tw - 24, 19, tag,
                    COL_ACCENT, COL_ACCENT_BG, COL_ACCENT_DARK);

    inst_blit_rect(cx + 2, cy + 6, cw - 4, hdr_strip_h, s_strip + 2, cw);

    /* Subtle bottom OS watermark outside card */
    if (cy + ch + 26 < fh) {
        int bw = cw;
        surf_fill(s_strip, bw, bw, 22, COL_BG_BOT);
        const char *wm = "Arctian OS (x86_64)  |  Kalici Disk Kurulum Sihirbazi";
        int wmw = inst_text_width(wm, 1);
        surf_draw_text(s_strip, bw, bw, 22, (bw - wmw) / 2, 2, wm, COL_TEXT_DIM, 1);
        inst_blit_rect(cx, cy + ch + 10, bw, 22, s_strip, bw);
    }
}

/* -------------------------------------------------------------------------
 * Screen 1: Disk Selection Rendering
 * ------------------------------------------------------------------------- */
static void inst_draw_disk_row(int vis_idx, int abs_idx, const inst_disk_t *d, bool selected) {
    int rw = g_lay.card_w - 48;
    int rh = 48;
    int rx = g_lay.card_x + 24;
    int ry = g_lay.body_y + 30 + vis_idx * 54;

    uint32_t bg  = selected ? COL_ACCENT_BG : COL_ROW_BG;
    uint32_t brd = selected ? COL_ACCENT    : COL_ROW_BORDER;

    surf_fill(s_strip, rw, rw, rh, COL_CARD_BG);
    surf_round_rect(s_strip, rw, rw, rh, 0, 0, rw, rh, 8, bg, brd);

    /* Left selection accent bar */
    if (selected) {
        surf_fill_rect(s_strip, rw, rw, rh, 1, 7, 4, rh - 14, COL_ACCENT);
    }

    /* Index badge [1]..[9] */
    char num[4];
    num[0] = (char)('1' + (abs_idx % 9));
    num[1] = '\0';
    surf_round_rect(s_strip, rw, rw, rh, 16, 11, 26, 26, 6,
                    selected ? COL_ACCENT : COL_CARD_HDR,
                    selected ? COL_ACCENT : COL_CARD_BORDER);
    surf_draw_text(s_strip, rw, rw, rh, 25, 16, num,
                   selected ? COL_BG_TOP : COL_TEXT_SEC, 1);

    /* Device name */
    surf_draw_text(s_strip, rw, rw, rh, 54, 16, d->info.name,
                   selected ? COL_TEXT_PRI : COL_TEXT_SEC, 1);

    /* Bus type badge */
    const char *bus_lbl = "ATA";
    uint32_t bus_col = COL_ATA;
    if (d->info.kind == BDEV_KIND_NVME) { bus_lbl = "NVMe"; bus_col = COL_NVME; }
    else if (d->info.kind == BDEV_KIND_AHCI) { bus_lbl = "SATA/AHCI"; bus_col = COL_SATA; }
    else if (d->info.kind == BDEV_KIND_USB) { bus_lbl = "USB"; bus_col = COL_USB; }
    surf_draw_badge(s_strip, rw, rw, rh, 152, 12, bus_lbl, bus_col, COL_CARD_BG, bus_col);

    /* Capacity text */
    char sz_str[32];
    inst_fmt_size(d->info.sectors, sz_str, sizeof(sz_str));
    surf_draw_text(s_strip, rw, rw, rh, 272, 16, sz_str, COL_TEXT_PRI, 1);

    /* Status badge on right */
    const char *st_lbl = "Hazir";
    uint32_t st_fg = COL_OK, st_bg = COL_OK_BG, st_brd = COL_OK;
    if (d->too_small) {
        st_lbl = "Alan yetersiz";
        st_fg = COL_ERR; st_bg = COL_ERR_BG; st_brd = COL_ERR;
    } else if (d->has_arctian) {
        st_lbl = "Mevcut sistem";
        st_fg = COL_WARN; st_bg = COL_WARN_BG; st_brd = COL_WARN;
    }
    int bw = inst_text_width(st_lbl, 1) + 16;
    int st_x = rw - bw - 14;

    /* Partition table badge ([GPT/EFI], [GPT], [MBR]) */
    const char *pt_lbl = 0;
    uint32_t pt_fg = COL_TEXT_SEC, pt_bg = COL_CARD_HDR, pt_brd = COL_CARD_BORDER;
    if (d->has_gpt && d->has_efi_part) {
        pt_lbl = "[GPT/EFI]";
        pt_fg = COL_WARN; pt_bg = COL_WARN_BG; pt_brd = COL_WARN;
    } else if (d->has_gpt) {
        pt_lbl = "[GPT]";
        pt_fg = COL_ACCENT; pt_bg = COL_ACCENT_BG; pt_brd = COL_ACCENT_DARK;
    } else if (d->has_mbr) {
        pt_lbl = "[MBR]";
        pt_fg = COL_TEXT_SEC; pt_bg = COL_CARD_HDR; pt_brd = COL_CARD_BORDER;
    }
    if (pt_lbl) {
        int pt_x = 272 + inst_text_width(sz_str, 1) + 12;
        int pt_w = inst_text_width(pt_lbl, 1) + 16;
        if (pt_x + pt_w + 8 <= st_x) {
            surf_draw_badge(s_strip, rw, rw, rh, pt_x, 12, pt_lbl, pt_fg, pt_bg, pt_brd);
        }
    }

    surf_draw_badge(s_strip, rw, rw, rh, st_x, 12, st_lbl, st_fg, st_bg, st_brd);

    inst_blit_rect(rx, ry, rw, rh, s_strip, rw);
}

static void inst_draw_picker_banner(const inst_disk_t *disks, int count, int sel,
                                    bool confirm, const char *err_msg, uint64_t req_secs) {
    if (!g_fb.active) return;

    int bw = g_lay.card_w - 48;
    int bx = g_lay.card_x + 24;

    /* Top info bar inside body */
    int top_h = 26;
    surf_fill(s_strip, bw, bw, top_h, COL_CARD_BG);
    char info_line[96];
    char *p = info_line;
    char *end = info_line + sizeof(info_line);
    p = inst_str_append(p, end, "Algilanan disk: ");
    p = inst_u64_append(p, end, (uint64_t)count);
    p = inst_str_append(p, end, "   |   Gereken en az alan: ");
    char req_sz[24];
    if (req_secs <= (~0ULL / 512ULL)) {
        inst_fmt_bytes(req_secs * 512ULL, req_sz, sizeof(req_sz));
    } else {
        inst_fmt_size(req_secs, req_sz, sizeof(req_sz));
    }
    inst_str_append(p, end, req_sz);
    surf_draw_text(s_strip, bw, bw, top_h, 2, 4, info_line, COL_TEXT_SEC, 1);
    inst_blit_rect(bx, g_lay.body_y, bw, top_h, s_strip, bw);

    /* Confirmation / Error / Hint footer strip */
    int fh = 48;
    int fy = g_lay.card_y + g_lay.card_h - fh - 16;
    surf_fill(s_strip, bw, bw, fh, COL_CARD_BG);

    if (err_msg && err_msg[0]) {
        surf_round_rect(s_strip, bw, bw, fh, 0, 0, bw, fh, 8, COL_ERR_BG, COL_ERR);
        surf_draw_text(s_strip, bw, bw, fh, 16, 16, err_msg, COL_ERR, 1);
    } else if (confirm && count > 0 && sel >= 0 && sel < count) {
        char msg[128];
        char *mp = msg;
        char *mend = msg + sizeof(msg);
        uint32_t c_bg = COL_WARN_BG, c_fg = COL_WARN, c_brd = COL_WARN;
        if (disks[sel].has_gpt && disks[sel].has_efi_part) {
            c_bg = COL_ERR_BG; c_fg = COL_ERR; c_brd = COL_ERR;
            mp = inst_str_append(mp, mend, "UYARI: GPT/EFI diski silinecek (");
            mp = inst_str_append(mp, mend, disks[sel].info.name);
            inst_str_append(mp, mend, ")! [ENTER] Onayla  [ESC] Iptal");
        } else if (disks[sel].has_gpt) {
            mp = inst_str_append(mp, mend, "UYARI: GPT diski silinecek (");
            mp = inst_str_append(mp, mend, disks[sel].info.name);
            inst_str_append(mp, mend, ")! [ENTER] Onayla  [ESC] Iptal");
        } else {
            mp = inst_str_append(mp, mend, "Secili disk silinecek (");
            mp = inst_str_append(mp, mend, disks[sel].info.name);
            inst_str_append(mp, mend, ")! Onay: [ENTER]   Geri: [ESC]");
        }
        surf_round_rect(s_strip, bw, bw, fh, 0, 0, bw, fh, 8, c_bg, c_brd);
        surf_draw_text(s_strip, bw, bw, fh, 14, 16, msg, c_fg, 1);
    } else {
        surf_round_rect(s_strip, bw, bw, fh, 0, 0, bw, fh, 8, COL_CARD_HDR, COL_ROW_BORDER);
        const char *hint = "[Yukari/Asagi] veya [1-9] Sec   [ENTER] Kurulum   [R] Yeniden Tara";
        int hw = inst_text_width(hint, 1);
        surf_draw_text(s_strip, bw, bw, fh, (bw - hw) / 2, 16, hint, COL_TEXT_SEC, 1);
    }

    inst_blit_rect(bx, fy, bw, fh, s_strip, bw);
}

static void inst_render_picker(const inst_disk_t *disks, int count, int sel, int scroll,
                               bool confirm, const char *err_msg, uint64_t req_secs,
                               bool full_redraw) {
    if (!g_fb.active) {
        if (full_redraw) {
            terminal_initialize();
            show_arctian_header();
            char req_sz[24];
            inst_fmt_size(req_secs, req_sz, sizeof(req_sz));
            kprintf("ARCTIAN OS KURULUM - Hedef diski secin (En az %s):\n", req_sz);
            for (int i = 0; i < count; i++) {
                char sz[24];
                inst_fmt_size(disks[i].info.sectors, sz, sizeof(sz));
                const char *pt = (disks[i].has_gpt && disks[i].has_efi_part) ? " [GPT/EFI]" :
                                 disks[i].has_gpt ? " [GPT]" :
                                 disks[i].has_mbr ? " [MBR]" : "";
                kprintf("  %s [%d] %s (%s)%s%s\n",
                        (i == sel) ? ">" : " ", i + 1, disks[i].info.name, sz, pt,
                        disks[i].too_small ? " [YETERSIZ]" : "");
            }
            if (confirm) {
                if (sel >= 0 && sel < count && disks[sel].has_gpt) {
                    kprintf("UYARI: %s diski silinecek! [ENTER] Onayla, [ESC] Iptal\n",
                            disks[sel].has_efi_part ? "GPT/EFI" : "GPT");
                } else {
                    terminal_writestring("Secili disk silinecek! [ENTER] Onayla, [ESC] Iptal\n");
                }
            }
        }
        return;
    }

    if (full_redraw) {
        inst_draw_shell("Hedef diski secin");
    }

    int bw = g_lay.card_w - 48;
    int bx = g_lay.card_x + 24;

    if (count <= 0) {
        int ah = 64;
        int ay = g_lay.body_y + 52;
        surf_fill(s_strip, bw, bw, ah, COL_CARD_BG);
        surf_round_rect(s_strip, bw, bw, ah, 0, 0, bw, ah, 8, COL_ERR_BG, COL_ERR);
        surf_draw_text(s_strip, bw, bw, ah, 18, 12,
                       "Kurulum icin uygun yazilabilir disk bulunamadi!", COL_ERR, 1);
        surf_draw_text(s_strip, bw, bw, ah, 18, 34,
                       "Disk baglayip [R] tusu ile yeniden tarayin.", COL_TEXT_PRI, 1);
        inst_blit_rect(bx, ay, bw, ah, s_strip, bw);
    } else {
        int vis = count - scroll;
        if (vis > INST_MAX_VISIBLE) vis = INST_MAX_VISIBLE;
        for (int v = 0; v < INST_MAX_VISIBLE; v++) {
            if (v < vis) {
                int idx = scroll + v;
                inst_draw_disk_row(v, idx, &disks[idx], idx == sel);
            } else if (full_redraw) {
                surf_fill(s_strip, bw, bw, 48, COL_CARD_BG);
                inst_blit_rect(bx, g_lay.body_y + 30 + v * 54, bw, 48, s_strip, bw);
            }
        }
    }

    inst_draw_picker_banner(disks, count, sel, confirm, err_msg, req_secs);
}

/* -------------------------------------------------------------------------
 * Screen 2 & 3: Real-Time Installation Progress, Success & Error UI
 * ------------------------------------------------------------------------- */
static void inst_render_progress_shell(const inst_disk_t *target) {
    if (!g_fb.active) {
        terminal_initialize();
        show_arctian_header();
        kprintf("Kurulum baslatiliyor: %s\n", target->info.name);
        return;
    }

    inst_draw_shell("Sistem hedef diske kuruluyor");

    int bw = g_lay.card_w - 48;
    int bx = g_lay.card_x + 24;

    /* Target summary box */
    int th = 52;
    surf_fill(s_strip, bw, bw, th, COL_CARD_BG);
    surf_round_rect(s_strip, bw, bw, th, 0, 0, bw, th, 8, COL_ROW_BG, COL_ROW_BORDER);

    char tline[96];
    char *p = tline;
    char *end = tline + sizeof(tline);
    p = inst_str_append(p, end, "Hedef Disk: ");
    p = inst_str_append(p, end, target->info.name);
    p = inst_str_append(p, end, "  (");
    char sz[24];
    inst_fmt_size(target->info.sectors, sz, sizeof(sz));
    p = inst_str_append(p, end, sz);
    inst_str_append(p, end, ")");
    surf_draw_text(s_strip, bw, bw, th, 16, 18, tline, COL_TEXT_PRI, 1);

    const char *bus_lbl = "ATA";
    uint32_t bus_col = COL_ATA;
    if (target->info.kind == BDEV_KIND_NVME) { bus_lbl = "NVMe"; bus_col = COL_NVME; }
    else if (target->info.kind == BDEV_KIND_AHCI) { bus_lbl = "SATA/AHCI"; bus_col = COL_SATA; }
    else if (target->info.kind == BDEV_KIND_USB) { bus_lbl = "USB"; bus_col = COL_USB; }
    int bdg_w = inst_text_width(bus_lbl, 1) + 16;
    surf_draw_badge(s_strip, bw, bw, th, bw - bdg_w - 14, 14, bus_lbl, bus_col, COL_CARD_BG, bus_col);

    inst_blit_rect(bx, g_lay.body_y + 6, bw, th, s_strip, bw);

    int fh = 48;
    int fy = g_lay.card_y + g_lay.card_h - fh - 16;
    surf_fill(s_strip, bw, bw, fh, COL_CARD_BG);
    surf_round_rect(s_strip, bw, bw, fh, 0, 0, bw, fh, 8, COL_CARD_HDR, COL_ROW_BORDER);
    const char *msg = "Kurulum suruyor, lutfen sistemi kapatmayin...";
    int mw = inst_text_width(msg, 1);
    surf_draw_text(s_strip, bw, bw, fh, (bw - mw) / 2, 16, msg, COL_TEXT_SEC, 1);
    inst_blit_rect(bx, fy, bw, fh, s_strip, bw);
}

static void inst_update_progress(uint32_t pct, uint64_t done_bytes, uint64_t total_bytes,
                                 const char *step_label, const char *detail_label, bool done_ok) {
    if (pct > 100u) pct = 100u;

    if (!g_fb.active) {
        char done_s[24], tot_s[24];
        inst_fmt_bytes(done_bytes, done_s, sizeof(done_s));
        inst_fmt_bytes(total_bytes, tot_s, sizeof(tot_s));
        kprintf("[%u%%] %s (%s / %s)\n", (unsigned)pct, step_label, done_s, tot_s);
        return;
    }

    int bw = g_lay.card_w - 48;
    int bx = g_lay.card_x + 24;

    /* Band 1: Step label + big percentage readout (height 56) */
    int b1h = 56;
    int b1y = g_lay.body_y + 78;
    surf_fill(s_strip, bw, bw, b1h, COL_CARD_BG);
    surf_draw_text(s_strip, bw, bw, b1h, 2, 10, step_label,
                   done_ok ? COL_OK : COL_TEXT_PRI, 1);
    if (detail_label && detail_label[0]) {
        surf_draw_text(s_strip, bw, bw, b1h, 2, 32, detail_label, COL_TEXT_SEC, 1);
    }

    char pct_str[8];
    char *pp = pct_str;
    pp = inst_u64_append(pp, pct_str + sizeof(pct_str), pct);
    inst_str_append(pp, pct_str + sizeof(pct_str), "%");
    int pw = inst_text_width(pct_str, 2);
    surf_draw_text(s_strip, bw, bw, b1h, bw - pw - 2, 12, pct_str,
                   done_ok ? COL_OK : COL_ACCENT, 2);
    inst_blit_rect(bx, b1y, bw, b1h, s_strip, bw);

    /* Band 2: Progress bar + MB written / total MB (height 64) */
    int b2h = 64;
    int b2y = b1y + 62;
    surf_fill(s_strip, bw, bw, b2h, COL_CARD_BG);

    int bar_h = 26;
    surf_round_rect(s_strip, bw, bw, b2h, 0, 0, bw, bar_h, 8, COL_TRACK, COL_ROW_BORDER);

    int inner_w = bw - 6;
    int fill_w = (int)(((uint64_t)inner_w * pct) / 100ULL);
    if (fill_w > 0) {
        if (fill_w < 10 && pct > 0) fill_w = 10;
        if (fill_w > inner_w) fill_w = inner_w;
        uint32_t c0 = done_ok ? COL_OK : COL_ACCENT_DARK;
        uint32_t c1 = done_ok ? COL_OK : COL_ACCENT;
        for (int py = 3; py < bar_h - 3; py++) {
            for (int px = 3; px < 3 + fill_w; px++) {
                if (!pt_in_rrect(px - 3, py - 3, fill_w, bar_h - 6, 5)) continue;
                s_strip[py * bw + px] = inst_lerp_color(c0, c1, px - 3, fill_w);
            }
        }
    }

    /* Written / Total (MB or GB cleanly via inst_fmt_bytes) */
    char mb_line[64];
    char done_s[24], tot_s[24];
    inst_fmt_bytes(done_bytes, done_s, sizeof(done_s));
    inst_fmt_bytes(total_bytes ? total_bytes : 1ULL, tot_s, sizeof(tot_s));
    char *mp = mb_line;
    char *mend = mb_line + sizeof(mb_line);
    mp = inst_str_append(mp, mend, done_s);
    mp = inst_str_append(mp, mend, " / ");
    mp = inst_str_append(mp, mend, tot_s);
    inst_str_append(mp, mend, " yazildi");
    surf_draw_text(s_strip, bw, bw, b2h, 2, 36, mb_line, COL_TEXT_SEC, 1);

    inst_blit_rect(bx, b2y, bw, b2h, s_strip, bw);
}

static void inst_render_status_card(bool is_error, const char *title, const char *subtitle,
                                    const char *footer_hint) {
    if (!g_fb.active) {
        kprintf("\n%s: %s\n%s\n", title, subtitle, footer_hint);
        return;
    }

    int bw = g_lay.card_w - 48;
    int bx = g_lay.card_x + 24;

    int ch = 64;
    int cy = g_lay.body_y + 216;
    uint32_t fg  = is_error ? COL_ERR    : COL_OK;
    uint32_t bg  = is_error ? COL_ERR_BG : COL_OK_BG;

    surf_fill(s_strip, bw, bw, ch, COL_CARD_BG);
    surf_round_rect(s_strip, bw, bw, ch, 0, 0, bw, ch, 8, bg, fg);
    surf_draw_text(s_strip, bw, bw, ch, 16, 10, title, fg, 1);
    surf_draw_text(s_strip, bw, bw, ch, 16, 34, subtitle, COL_TEXT_PRI, 1);
    inst_blit_rect(bx, cy, bw, ch, s_strip, bw);

    int fh = 48;
    int fy = g_lay.card_y + g_lay.card_h - fh - 16;
    surf_fill(s_strip, bw, bw, fh, COL_CARD_BG);
    surf_round_rect(s_strip, bw, bw, fh, 0, 0, bw, fh, 8, COL_CARD_HDR, COL_ROW_BORDER);
    int hw = inst_text_width(footer_hint, 1);
    surf_draw_text(s_strip, bw, bw, fh, (bw - hw) / 2, 16, footer_hint, COL_TEXT_SEC, 1);
    inst_blit_rect(bx, fy, bw, fh, s_strip, bw);
}

/* -------------------------------------------------------------------------
 * PS/2 Keyboard Polling (no IRQ1 interference while PIC is masked)
 * ------------------------------------------------------------------------- */
static void inst_kbd_flush(void) {
    s_e0_prefix = false;
    for (int i = 0; i < 64; i++) {
        if (!(inb(0x64) & 0x01u)) break;
        (void)inb(0x60);
    }
}

static inst_key_t inst_poll_key(void) {
    while (inb(0x64) & 0x01u) {
        uint8_t st = inb(0x64);
        uint8_t sc = inb(0x60);
        if (st & 0x20u) {
            /* Auxiliary (mouse) byte — ignore */
            continue;
        }
        if (sc == 0xE0u) {
            s_e0_prefix = true;
            continue;
        }
        if (sc == 0xE1u) {
            s_e0_prefix = false;
            continue;
        }

        bool ext = s_e0_prefix;
        s_e0_prefix = false;

        if (sc & 0x80u) continue; /* key release */

        if (ext) {
            if (sc == 0x48u) return INST_KEY_UP;
            if (sc == 0x50u) return INST_KEY_DOWN;
            if (sc == 0x1Cu) return INST_KEY_ENTER;
            continue;
        }

        switch (sc) {
            case 0x1Cu: return INST_KEY_ENTER;
            case 0x01u: return INST_KEY_ESC;
            case 0x11u: return INST_KEY_UP;     /* W */
            case 0x1Fu: return INST_KEY_DOWN;   /* S */
            case 0x48u: return INST_KEY_UP;     /* KP 8 / Up */
            case 0x50u: return INST_KEY_DOWN;   /* KP 2 / Down */
            case 0x13u: return INST_KEY_RESCAN; /* R */
            case 0x02u: return INST_KEY_1;
            case 0x03u: return INST_KEY_2;
            case 0x04u: return INST_KEY_3;
            case 0x05u: return INST_KEY_4;
            case 0x06u: return INST_KEY_5;
            case 0x07u: return INST_KEY_6;
            case 0x08u: return INST_KEY_7;
            case 0x09u: return INST_KEY_8;
            case 0x0Au: return INST_KEY_9;
            default:    break;
        }
    }
    return INST_KEY_NONE;
}

static inst_key_t inst_wait_key(void) {
    for (;;) {
        inst_key_t k = inst_poll_key();
        if (k != INST_KEY_NONE) return k;
        for (int i = 0; i < 40000; i++) asm volatile("pause");
    }
}

static void inst_reboot(void) {
    for (int i = 0; i < 1000; i++) {
        if (!(inb(0x64) & 0x02u)) {
            outb(0x64, 0xFEu);
            break;
        }
    }
    outb(0xCF9, 0x06u);
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) null_idt = {0, 0};
    asm volatile("lidt %0; int $3" :: "m"(null_idt));
    for (;;) asm volatile("hlt");
}

#endif /* ARCTIAN_INST_UI_H */
