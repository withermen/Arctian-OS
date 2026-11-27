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
 * Arctian OS Release Graphical Installer UI
 *  - Zero-alloc, flicker-free offscreen strip blitting
 *  - Turkish UTF-8 8x8 bitmap glyph extension
 *  - PS/2 mouse packet decoder, software cursor & button/row hit-testing
 *  - Release ISO strictly installer-only (no Live Desktop button)
 * ========================================================================= */

#define INST_MAX_DISKS      BLOCKDEV_MAX
#define INST_MAX_VISIBLE    4
#define INST_MAX_CARD_W     760
#define INST_STRIP_H        96

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

typedef enum {
    INST_KEY_NONE = 0,
    INST_KEY_UP,
    INST_KEY_DOWN,
    INST_KEY_LEFT,
    INST_KEY_RIGHT,
    INST_KEY_ENTER,
    INST_KEY_ESC,
    INST_KEY_RESCAN,
    INST_KEY_MODE,
    INST_KEY_DUALBOOT,
    INST_KEY_METHOD_CLEAN,
    INST_KEY_METHOD_DUAL,
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

typedef enum {
    INST_SCHEME_UEFI_GPT = 0,
    INST_SCHEME_BIOS_MBR = 1
} inst_scheme_t;

typedef enum {
    INST_METHOD_CLEAN    = 0,
    INST_METHOD_DUALBOOT = 1
} inst_method_t;

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
    bool            can_dualboot;
    uint64_t        free_start_lba;
    uint64_t        free_sectors;
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

typedef struct {
    int  x, y, w, h;
    bool active;
} inst_rect_t;

typedef enum {
    INST_UI_SCREEN_NONE = 0,
    INST_UI_SCREEN_PICKER,
    INST_UI_SCREEN_PROGRESS,
    INST_UI_SCREEN_STATUS
} inst_ui_screen_t;

/* Dedicated page-table storage so framebuffer mapping never collides with
 * mmio_reset() inside blockdev_init(). */
static uint64_t s_fb_pdpt[512] __attribute__((aligned(4096)));
static uint64_t s_fb_pd[4][512] __attribute__((aligned(4096)));
static int      s_fb_pd_used = 0;

/* Offscreen composition buffers */
static uint32_t s_strip[INST_MAX_CARD_W * INST_STRIP_H];
static uint32_t s_scanline[3840];

static inst_fb_t     g_fb;
static inst_layout_t g_lay;
static bool          s_e0_prefix = false;

/* Mouse & clickable hitbox state */
#define INST_CUR_W 12
#define INST_CUR_H 16
static bool             s_mouse_ready = false;
static bool             s_mouse_drawn = false;
static int              s_mouse_x = 0;
static int              s_mouse_y = 0;
static int              s_mouse_drawn_x = 0;
static int              s_mouse_drawn_y = 0;
static bool             s_mouse_left_prev = false;
static uint8_t          s_mouse_pkt[3];
static int              s_mouse_cycle = 0;
static uint32_t         s_cur_save[INST_CUR_W * INST_CUR_H];

static inst_ui_screen_t s_ui_screen = INST_UI_SCREEN_NONE;
static inst_rect_t      s_box_rows[INST_MAX_VISIBLE];
static int              s_box_row_abs[INST_MAX_VISIBLE];
static inst_rect_t      s_box_mode;
static inst_rect_t      s_box_clean;
static inst_rect_t      s_box_dual;
static inst_rect_t      s_box_rescan;
static inst_rect_t      s_box_primary;
static inst_rect_t      s_box_cancel;
static int              s_clicked_disk_abs = -1;

/* -------------------------------------------------------------------------
 * Low-level helpers (color, string, UTF-8, time)
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
    for (int i = 0; i < 10000; i++) {
        outb(0x70, 0x0Au);
        if (!(inb(0x71) & 0x80u)) break;
    }
    outb(0x70, 0x00u);
    return inb(0x71);
}

/* -------------------------------------------------------------------------
 * Turkish UTF-8 8x8 Bitmap Font Extension
 * ------------------------------------------------------------------------- */
static const uint8_t s_tr_glyphs[12][8] = {
    /* 0: Ç (U+00C7) */ {0x3C,0x66,0x60,0x60,0x66,0x3C,0x18,0x30},
    /* 1: ç (U+00E7) */ {0x00,0x00,0x3C,0x66,0x60,0x3C,0x18,0x30},
    /* 2: Ğ (U+011E) */ {0x42,0x3C,0x00,0x3C,0x60,0x6E,0x66,0x3C},
    /* 3: ğ (U+011F) */ {0x42,0x3C,0x00,0x3E,0x66,0x3E,0x06,0x3C},
    /* 4: İ (U+0130) */ {0x18,0x00,0x7E,0x18,0x18,0x18,0x7E,0x00},
    /* 5: ı (U+0131) */ {0x00,0x00,0x38,0x18,0x18,0x18,0x3C,0x00},
    /* 6: Ö (U+00D6) */ {0x66,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00},
    /* 7: ö (U+00F6) */ {0x66,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00},
    /* 8: Ş (U+015E) */ {0x3C,0x60,0x3C,0x06,0x66,0x3C,0x18,0x30},
    /* 9: ş (U+015F) */ {0x00,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x30},
    /*10: Ü (U+00DC) */ {0x66,0x00,0x66,0x66,0x66,0x66,0x3C,0x00},
    /*11: ü (U+00FC) */ {0x66,0x00,0x66,0x66,0x66,0x66,0x3E,0x00}
};

static const uint8_t s_arrow_glyphs[5][8] = {
    /* 0: ↑ (U+2191) */ {0x18,0x3C,0x7E,0x18,0x18,0x18,0x18,0x00},
    /* 1: ↓ (U+2193) */ {0x18,0x18,0x18,0x18,0x7E,0x3C,0x18,0x00},
    /* 2: ← (U+2190) */ {0x00,0x18,0x30,0x7E,0x30,0x18,0x00,0x00},
    /* 3: → (U+2192) */ {0x00,0x18,0x0C,0x7E,0x0C,0x18,0x00,0x00},
    /* 4: ↵ (U+21B5) */ {0x04,0x04,0x04,0x24,0x64,0xFC,0x60,0x20}
};

static uint32_t inst_utf8_next(const char **ps) {
    const unsigned char *s = (const unsigned char *)*ps;
    unsigned char c = s[0];
    if (c == 0) return 0;
    if (c < 0x80u) {
        (*ps)++;
        return c;
    }
    if ((c & 0xE0u) == 0xC0u && (s[1] & 0xC0u) == 0x80u) {
        uint32_t cp = ((uint32_t)(c & 0x1Fu) << 6) | (uint32_t)(s[1] & 0x3Fu);
        *ps += 2;
        return cp;
    }
    if ((c & 0xF0u) == 0xE0u && (s[1] & 0xC0u) == 0x80u && (s[2] & 0xC0u) == 0x80u) {
        uint32_t cp = ((uint32_t)(c & 0x0Fu) << 12) |
                      ((uint32_t)(s[1] & 0x3Fu) << 6) |
                      (uint32_t)(s[2] & 0x3Fu);
        *ps += 3;
        return cp;
    }
    if ((c & 0xF8u) == 0xF0u && (s[1] & 0xC0u) == 0x80u &&
        (s[2] & 0xC0u) == 0x80u && (s[3] & 0xC0u) == 0x80u) {
        *ps += 4;
        return '?';
    }
    (*ps)++;
    return '?';
}

static const uint8_t *inst_glyph(uint32_t cp) {
    if (cp >= 32u && cp <= 126u) return font8x8_basic[cp - 32u];
    switch (cp) {
        case 0x00C7u: return s_tr_glyphs[0];  /* Ç */
        case 0x00E7u: return s_tr_glyphs[1];  /* ç */
        case 0x011Eu: return s_tr_glyphs[2];  /* Ğ */
        case 0x011Fu: return s_tr_glyphs[3];  /* ğ */
        case 0x0130u: return s_tr_glyphs[4];  /* İ */
        case 0x0131u: return s_tr_glyphs[5];  /* ı */
        case 0x00D6u: return s_tr_glyphs[6];  /* Ö */
        case 0x00F6u: return s_tr_glyphs[7];  /* ö */
        case 0x015Eu: return s_tr_glyphs[8];  /* Ş */
        case 0x015Fu: return s_tr_glyphs[9];  /* ş */
        case 0x00DCu: return s_tr_glyphs[10]; /* Ü */
        case 0x00FCu: return s_tr_glyphs[11]; /* ü */
        case 0x2191u: return s_arrow_glyphs[0];
        case 0x2193u: return s_arrow_glyphs[1];
        case 0x2190u: return s_arrow_glyphs[2];
        case 0x2192u: return s_arrow_glyphs[3];
        case 0x21B5u: return s_arrow_glyphs[4];
        case 0x2014u: return font8x8_basic['-' - 32u]; /* — */
        default:      return font8x8_basic['?' - 32u];
    }
}

/* -------------------------------------------------------------------------
 * Framebuffer Discovery, Page Mapping, Pixel Blit & Software Cursor
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
        if (pdpt[i3] & 0x80ULL) continue;
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

static inline uint32_t inst_pack_pixel(uint32_t rgb) {
    if (!g_fb.is_rgb) return rgb;
    return ((rgb & 0xFFu) << 16) | (rgb & 0x00FF00u) | ((rgb >> 16) & 0xFFu);
}

static inline uint32_t inst_fb_read_px(int x, int y) {
    if (x < 0 || y < 0 || x >= (int)g_fb.width || y >= (int)g_fb.height) return 0;
    if (g_fb.bpp == 32) {
        volatile uint32_t *p = (volatile uint32_t *)(g_fb.addr + (size_t)y * g_fb.pitch) + x;
        uint32_t raw = *p;
        if (!g_fb.is_rgb) return raw;
        return ((raw & 0xFFu) << 16) | (raw & 0x00FF00u) | ((raw >> 16) & 0xFFu);
    }
    volatile uint8_t *p = g_fb.addr + (size_t)y * g_fb.pitch + (size_t)x * 3u;
    if (g_fb.is_rgb) return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    return ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

static inline void inst_fb_write_px(int x, int y, uint32_t rgb) {
    if (x < 0 || y < 0 || x >= (int)g_fb.width || y >= (int)g_fb.height) return;
    if (g_fb.bpp == 32) {
        volatile uint32_t *p = (volatile uint32_t *)(g_fb.addr + (size_t)y * g_fb.pitch) + x;
        *p = inst_pack_pixel(rgb);
    } else if (g_fb.bpp == 24) {
        volatile uint8_t *p = g_fb.addr + (size_t)y * g_fb.pitch + (size_t)x * 3u;
        if (g_fb.is_rgb) {
            p[0] = (uint8_t)(rgb >> 16); p[1] = (uint8_t)(rgb >> 8); p[2] = (uint8_t)rgb;
        } else {
            p[0] = (uint8_t)rgb; p[1] = (uint8_t)(rgb >> 8); p[2] = (uint8_t)(rgb >> 16);
        }
    }
}

/* 12x16 arrow cursor: 0=transparent, 1=border(dark), 2=fill(white) */
static const uint8_t s_cursor_art[INST_CUR_H][INST_CUR_W] = {
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0},
    {1,2,2,2,2,2,2,2,2,1,0,0},
    {1,2,2,2,2,2,1,1,1,1,1,0},
    {1,2,2,1,2,2,1,0,0,0,0,0},
    {1,2,1,0,1,2,2,1,0,0,0,0},
    {1,1,0,0,1,2,2,1,0,0,0,0},
    {0,0,0,0,0,1,2,1,0,0,0,0},
    {0,0,0,0,0,0,1,0,0,0,0,0}
};

static void inst_cursor_hide(void) {
    if (!g_fb.active || !s_mouse_drawn) return;
    for (int r = 0; r < INST_CUR_H; r++) {
        for (int c = 0; c < INST_CUR_W; c++) {
            if (s_cursor_art[r][c] != 0) {
                inst_fb_write_px(s_mouse_drawn_x + c, s_mouse_drawn_y + r,
                                 s_cur_save[r * INST_CUR_W + c]);
            }
        }
    }
    s_mouse_drawn = false;
}

static void inst_cursor_show(void) {
    if (!g_fb.active || !s_mouse_ready) return;
    if (s_mouse_drawn && s_mouse_drawn_x == s_mouse_x && s_mouse_drawn_y == s_mouse_y) return;
    if (s_mouse_drawn) inst_cursor_hide();

    s_mouse_drawn_x = s_mouse_x;
    s_mouse_drawn_y = s_mouse_y;
    for (int r = 0; r < INST_CUR_H; r++) {
        for (int c = 0; c < INST_CUR_W; c++) {
            if (s_cursor_art[r][c] != 0) {
                s_cur_save[r * INST_CUR_W + c] =
                    inst_fb_read_px(s_mouse_drawn_x + c, s_mouse_drawn_y + r);
                inst_fb_write_px(s_mouse_drawn_x + c, s_mouse_drawn_y + r,
                                 s_cursor_art[r][c] == 1 ? 0x05080Fu : 0xF8FAFCu);
            }
        }
    }
    s_mouse_drawn = true;
}

static void inst_blit_rect(int x, int y, int w, int h, const uint32_t *src, int src_stride) {
    if (!g_fb.active || w <= 0 || h <= 0) return;
    bool restore_cur = s_mouse_drawn;
    if (restore_cur) inst_cursor_hide();

    if (x < 0) { src -= x; w += x; x = 0; }
    if (y < 0) { src -= y * src_stride; h += y; y = 0; }
    if (x + w > (int)g_fb.width)  w = (int)g_fb.width - x;
    if (y + h > (int)g_fb.height) h = (int)g_fb.height - y;
    if (w > 0 && h > 0) {
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

    if (restore_cur) inst_cursor_show();
}

/* -------------------------------------------------------------------------
 * PS/2 Mouse Init (Polling-only, IRQ12 masked)
 * ------------------------------------------------------------------------- */
static bool inst_ps2_wait_write(void) {
    for (int i = 0; i < 60000; i++) {
        if ((inb(0x64) & 0x02u) == 0) return true;
    }
    return false;
}

static bool inst_ps2_wait_read(void) {
    for (int i = 0; i < 60000; i++) {
        if ((inb(0x64) & 0x01u) != 0) return true;
    }
    return false;
}

static void inst_ps2_drain(void) {
    for (int i = 0; i < 64; i++) {
        if (!(inb(0x64) & 0x01u)) break;
        (void)inb(0x60);
    }
}

static bool inst_mouse_send(uint8_t cmd, uint8_t *ack) {
    if (!inst_ps2_wait_write()) return false;
    outb(0x64, 0xD4u);
    if (!inst_ps2_wait_write()) return false;
    outb(0x60, cmd);
    if (!inst_ps2_wait_read()) return false;
    uint8_t v = inb(0x60);
    if (ack) *ack = v;
    return v == 0xFAu;
}

static void inst_mouse_init(void) {
    s_mouse_ready = false;
    s_mouse_drawn = false;
    s_mouse_cycle = 0;
    s_mouse_left_prev = false;
    if (!g_fb.active) return;

    s_mouse_x = (int)g_fb.width / 2;
    s_mouse_y = (int)g_fb.height / 2;

    inst_ps2_drain();
    if (!inst_ps2_wait_write()) return;
    outb(0x64, 0xA8u); /* Enable auxiliary PS/2 port 2 */
    inst_ps2_drain();

    if (inst_ps2_wait_write()) {
        outb(0x64, 0x20u);
        if (inst_ps2_wait_read()) {
            uint8_t cfg = inb(0x60);
            cfg &= (uint8_t)~0x22u; /* Enable port 2 clock (clear bit5), keep IRQ12 off (clear bit1) */
            if (inst_ps2_wait_write()) {
                outb(0x64, 0x60u);
                if (inst_ps2_wait_write()) outb(0x60, cfg);
            }
        }
    }
    inst_ps2_drain();

    uint8_t ack = 0;
    if (inst_mouse_send(0xFFu, &ack)) {
        /* Read 0xAA self-test and 0x00 device id */
        if (inst_ps2_wait_read()) (void)inb(0x60);
        if (inst_ps2_wait_read()) (void)inb(0x60);
    }
    inst_mouse_send(0xF6u, &ack); /* Set defaults */
    if (inst_mouse_send(0xF4u, &ack)) { /* Enable data reporting */
        s_mouse_ready = true;
    }
    inst_ps2_drain();
}

static bool inst_fb_init(void) {
    g_fb.active = 0;
    const arctian_bootinfo_t *bi = arctian_bootinfo();

    if (arctian_bootinfo_valid(bi) && bi->width >= 640u && bi->height >= 480u) {
        uint64_t fb_phys = (uint64_t)bi->fb_phys;
        uint64_t sz = bi->fb_size ? (uint64_t)bi->fb_size : 0ULL;
        if (arctian_bootinfo_is_uefi(bi)) {
            const volatile uint32_t *ui32 = (const volatile uint32_t *)(uintptr_t)0x7100u;
            if ((ui32[0] == 0x49464555u || ui32[0] == 2u) && ui32[1] >= 0x60u) {
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

    int ch = 504;
    if ((int)g_fb.height - 56 < ch) ch = (int)g_fb.height - 28;

    g_lay.card_w = cw;
    g_lay.card_h = ch;
    g_lay.card_x = ((int)g_fb.width - cw) / 2;
    g_lay.card_y = ((int)g_fb.height - ch) / 2;
    g_lay.hdr_h  = 72;
    g_lay.body_y = g_lay.card_y + g_lay.hdr_h + 12;
    g_lay.ftr_y  = g_lay.card_y + ch - 66;

    inst_mouse_init();
    return true;
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

static void surf_draw_glyph(uint32_t *buf, int stride, int bw, int bh,
                            int x, int y, uint32_t cp, uint32_t fg, int scale) {
    const uint8_t *g = inst_glyph(cp);
    if (scale >= 2) {
        for (int row = 0; row < 8; row++) {
            uint8_t cur = g[row];
            uint8_t up  = (row > 0) ? g[row - 1] : 0;
            uint8_t dn  = (row < 7) ? g[row + 1] : 0;
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
        for (int row = 0; row < 8; row++) {
            uint8_t bits = g[row];
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
    const char *p = s;
    while (*p) {
        (void)inst_utf8_next(&p);
        n++;
    }
    return n * (scale >= 2 ? 16 : 9);
}

static void surf_draw_text(uint32_t *buf, int stride, int bw, int bh,
                           int x, int y, const char *s, uint32_t fg, int scale) {
    int adv = (scale >= 2) ? 16 : 9;
    const char *p = s;
    while (*p) {
        uint32_t cp = inst_utf8_next(&p);
        surf_draw_glyph(buf, stride, bw, bh, x, y, cp, fg, scale);
        x += adv;
    }
}

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

static void surf_draw_button(uint32_t *buf, int stride, int bw, int bh,
                             int x, int y, int w, int h, const char *label,
                             uint32_t fg, uint32_t bg, uint32_t border) {
    surf_round_rect(buf, stride, bw, bh, x, y, w, h, 8, bg, border);
    int tw = inst_text_width(label, 1);
    int tx = x + (w - tw) / 2;
    int ty = y + (h - 16) / 2;
    surf_draw_text(buf, stride, bw, bh, tx, ty, label, fg, 1);
}

static void inst_clear_boxes(void) {
    for (int i = 0; i < INST_MAX_VISIBLE; i++) {
        s_box_rows[i].active = false;
        s_box_row_abs[i] = -1;
    }
    s_box_mode.active = false;
    s_box_clean.active = false;
    s_box_dual.active = false;
    s_box_rescan.active = false;
    s_box_primary.active = false;
    s_box_cancel.active = false;
}

static bool inst_pt_in_rect(int x, int y, const inst_rect_t *r) {
    return r->active && x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

/* -------------------------------------------------------------------------
 * Full Frame Background + Card Shell + Header
 * ------------------------------------------------------------------------- */
static void inst_draw_shell(const char *subtitle) {
    if (!g_fb.active) return;

    int fw = (int)g_fb.width;
    int fh = (int)g_fb.height;
    int line_w = fw < 3840 ? fw : 3840;
    int cx = g_lay.card_x, cy = g_lay.card_y;
    int cw = g_lay.card_w, ch = g_lay.card_h;

    inst_cursor_hide();

    for (int y = 0; y < fh; y++) {
        uint32_t bg = (y < 3) ? COL_ACCENT : inst_lerp_color(COL_BG_TOP, COL_BG_BOT, y, fh);
        for (int x = 0; x < line_w; x++) s_scanline[x] = bg;

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

    int hdr_strip_h = 62;
    surf_fill(s_strip, cw, cw, hdr_strip_h, COL_CARD_HDR);
    surf_round_rect(s_strip, cw, cw, hdr_strip_h, 24, 12, 38, 38, 8, COL_ACCENT_BG, COL_ACCENT);
    surf_draw_text(s_strip, cw, cw, hdr_strip_h, 35, 23, "A", COL_ACCENT, 2);

    surf_draw_text(s_strip, cw, cw, hdr_strip_h, 76, 12, "ARCTIAN OS KURULUM", COL_TEXT_PRI, 2);
    surf_draw_text(s_strip, cw, cw, hdr_strip_h, 76, 36, subtitle, COL_TEXT_SEC, 1);

    const char *tag = "CANLI ISO";
    int tw = inst_text_width(tag, 1) + 18;
    surf_draw_badge(s_strip, cw, cw, hdr_strip_h, cw - tw - 24, 19, tag,
                    COL_ACCENT, COL_ACCENT_BG, COL_ACCENT_DARK);

    inst_blit_rect(cx + 2, cy + 6, cw - 4, hdr_strip_h, s_strip + 2, cw);

    if (cy + ch + 26 < fh) {
        int bw = cw;
        surf_fill(s_strip, bw, bw, 22, COL_BG_BOT);
        const char *wm = "Arctian OS (x86_64)  |  Kalıcı Disk Kurulum Sihirbazı";
        int wmw = inst_text_width(wm, 1);
        surf_draw_text(s_strip, bw, bw, 22, (bw - wmw) / 2, 2, wm, COL_TEXT_DIM, 1);
        inst_blit_rect(cx, cy + ch + 10, bw, 22, s_strip, bw);
    }
    inst_cursor_show();
}

/* -------------------------------------------------------------------------
 * Screen 1: Disk Selection & Mode / Dual-Boot Rendering
 * ------------------------------------------------------------------------- */
static void inst_draw_disk_row(int vis_idx, int abs_idx, const inst_disk_t *d, bool selected) {
    int rw = g_lay.card_w - 48;
    int rh = 48;
    int rx = g_lay.card_x + 24;
    int ry = g_lay.body_y + 34 + vis_idx * 52;

    if (vis_idx >= 0 && vis_idx < INST_MAX_VISIBLE) {
        s_box_rows[vis_idx].x = rx;
        s_box_rows[vis_idx].y = ry;
        s_box_rows[vis_idx].w = rw;
        s_box_rows[vis_idx].h = rh;
        s_box_rows[vis_idx].active = true;
        s_box_row_abs[vis_idx] = abs_idx;
    }

    uint32_t bg  = selected ? COL_ACCENT_BG : COL_ROW_BG;
    uint32_t brd = selected ? COL_ACCENT    : COL_ROW_BORDER;

    surf_fill(s_strip, rw, rw, rh, COL_CARD_BG);
    surf_round_rect(s_strip, rw, rw, rh, 0, 0, rw, rh, 8, bg, brd);

    if (selected) {
        surf_fill_rect(s_strip, rw, rw, rh, 1, 7, 4, rh - 14, COL_ACCENT);
    }

    char num[4];
    num[0] = (char)('1' + (abs_idx % 9));
    num[1] = '\0';
    surf_round_rect(s_strip, rw, rw, rh, 14, 11, 26, 26, 6,
                    selected ? COL_ACCENT : COL_CARD_HDR,
                    selected ? COL_ACCENT : COL_CARD_BORDER);
    surf_draw_text(s_strip, rw, rw, rh, 23, 16, num,
                   selected ? COL_BG_TOP : COL_TEXT_SEC, 1);

    surf_draw_text(s_strip, rw, rw, rh, 50, 16, d->info.name,
                   selected ? COL_TEXT_PRI : COL_TEXT_SEC, 1);

    const char *bus_lbl = "ATA";
    uint32_t bus_col = COL_ATA;
    if (d->info.kind == BDEV_KIND_NVME) { bus_lbl = "NVMe"; bus_col = COL_NVME; }
    else if (d->info.kind == BDEV_KIND_AHCI) { bus_lbl = "SATA/AHCI"; bus_col = COL_SATA; }
    else if (d->info.kind == BDEV_KIND_USB) { bus_lbl = "USB"; bus_col = COL_USB; }
    surf_draw_badge(s_strip, rw, rw, rh, 144, 12, bus_lbl, bus_col, COL_CARD_BG, bus_col);

    char sz_str[32];
    inst_fmt_size(d->info.sectors, sz_str, sizeof(sz_str));
    surf_draw_text(s_strip, rw, rw, rh, 254, 16, sz_str, COL_TEXT_PRI, 1);

    const char *st_lbl = "Hazır";
    uint32_t st_fg = COL_OK, st_bg = COL_OK_BG, st_brd = COL_OK;
    if (d->too_small) {
        st_lbl = "Alan yetersiz";
        st_fg = COL_ERR; st_bg = COL_ERR_BG; st_brd = COL_ERR;
    } else if (d->can_dualboot) {
        st_lbl = "Dual-Boot uygun";
        st_fg = COL_OK; st_bg = COL_OK_BG; st_brd = COL_OK;
    } else if (d->has_arctian) {
        st_lbl = "Mevcut sistem";
        st_fg = COL_WARN; st_bg = COL_WARN_BG; st_brd = COL_WARN;
    }
    int bw = inst_text_width(st_lbl, 1) + 16;
    int st_x = rw - bw - 12;

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
        int pt_x = 254 + inst_text_width(sz_str, 1) + 10;
        int pt_w = inst_text_width(pt_lbl, 1) + 16;
        if (pt_x + pt_w + 8 <= st_x) {
            surf_draw_badge(s_strip, rw, rw, rh, pt_x, 12, pt_lbl, pt_fg, pt_bg, pt_brd);
        }
    }

    surf_draw_badge(s_strip, rw, rw, rh, st_x, 12, st_lbl, st_fg, st_bg, st_brd);
    inst_blit_rect(rx, ry, rw, rh, s_strip, rw);
}

static void inst_draw_picker_panels(const inst_disk_t *disks, int count, int sel,
                                    inst_scheme_t scheme, inst_method_t method,
                                    bool confirm, const char *err_msg, uint64_t req_secs) {
    if (!g_fb.active) return;

    int bw = g_lay.card_w - 48;
    int bx = g_lay.card_x + 24;

    /* Top info bar + [M] Boot/Partition Scheme toggle button */
    int top_h = 30;
    surf_fill(s_strip, bw, bw, top_h, COL_CARD_BG);

    char info_line[96];
    char *p = info_line;
    char *end = info_line + sizeof(info_line);
    p = inst_str_append(p, end, "Algılanan disk: ");
    p = inst_u64_append(p, end, (uint64_t)count);
    p = inst_str_append(p, end, "   |   En az alan: ");
    char req_sz[24];
    if (req_secs <= (~0ULL / 512ULL)) inst_fmt_bytes(req_secs * 512ULL, req_sz, sizeof(req_sz));
    else inst_fmt_size(req_secs, req_sz, sizeof(req_sz));
    inst_str_append(p, end, req_sz);
    surf_draw_text(s_strip, bw, bw, top_h, 2, 6, info_line, COL_TEXT_SEC, 1);

    const char *mode_lbl = (method == INST_METHOD_DUALBOOT && count > 0 && disks[sel].can_dualboot)
                           ? "Mod: GPT (Dual-Boot)"
                           : (scheme == INST_SCHEME_UEFI_GPT ? "[M] Mod: UEFI (GPT)" : "[M] Mod: BIOS (MBR)");
    int mode_w = inst_text_width(mode_lbl, 1) + 20;
    int mode_x = bw - mode_w;
    surf_draw_button(s_strip, bw, bw, top_h, mode_x, 1, mode_w, 26, mode_lbl,
                     scheme == INST_SCHEME_UEFI_GPT ? COL_ACCENT : COL_ATA,
                     COL_CARD_HDR,
                     scheme == INST_SCHEME_UEFI_GPT ? COL_ACCENT_DARK : COL_ROW_BORDER);
    if (!(method == INST_METHOD_DUALBOOT && count > 0 && disks[sel].can_dualboot)) {
        s_box_mode.x = bx + mode_x;
        s_box_mode.y = g_lay.body_y + 1;
        s_box_mode.w = mode_w;
        s_box_mode.h = 26;
        s_box_mode.active = true;
    } else {
        s_box_mode.active = false;
    }
    inst_blit_rect(bx, g_lay.body_y, bw, top_h, s_strip, bw);

    /* Method panel (y = body_y + 248, height = 84) */
    int mh = 84;
    int my = g_lay.body_y + 248;
    surf_fill(s_strip, bw, bw, mh, COL_CARD_BG);
    s_box_clean.active = false;
    s_box_dual.active = false;

    if (count > 0 && sel >= 0 && sel < count && disks[sel].can_dualboot) {
        char free_sz[24];
        inst_fmt_size(disks[sel].free_sectors, free_sz, sizeof(free_sz));
        char mhdr[96];
        char *mp = mhdr;
        char *mend = mhdr + sizeof(mhdr);
        mp = inst_str_append(mp, mend, "Kurulum Yöntemi ([D] / fare ile seç):  Ayrılmamış boş alan ");
        inst_str_append(mp, mend, free_sz);
        surf_draw_text(s_strip, bw, bw, mh, 2, 2, mhdr, COL_TEXT_SEC, 1);

        int half = (bw - 12) / 2;
        bool is_clean = (method == INST_METHOD_CLEAN);
        bool is_dual  = (method == INST_METHOD_DUALBOOT);

        surf_round_rect(s_strip, bw, bw, mh, 0, 24, half, 56, 8,
                        is_clean ? COL_ACCENT_BG : COL_ROW_BG,
                        is_clean ? COL_ACCENT    : COL_ROW_BORDER);
        surf_draw_text(s_strip, bw, bw, mh, 14, 32, "[1] Tüm Diski Sil ve Kur",
                       is_clean ? COL_TEXT_PRI : COL_TEXT_SEC, 1);
        surf_draw_text(s_strip, bw, bw, mh, 14, 54, "Disk sıfırlanır (temiz kurulum)",
                       is_clean ? COL_WARN : COL_TEXT_DIM, 1);
        s_box_clean.x = bx; s_box_clean.y = my + 24;
        s_box_clean.w = half; s_box_clean.h = 56; s_box_clean.active = true;

        int rx2 = half + 12;
        int rw2 = bw - rx2;
        surf_round_rect(s_strip, bw, bw, mh, rx2, 24, rw2, 56, 8,
                        is_dual ? COL_OK_BG : COL_ROW_BG,
                        is_dual ? COL_OK    : COL_ROW_BORDER);
        surf_draw_text(s_strip, bw, bw, mh, rx2 + 14, 32, "[2] Yanına Kur (Dual-Boot)",
                       is_dual ? COL_OK : COL_TEXT_PRI, 1);
        surf_draw_text(s_strip, bw, bw, mh, rx2 + 14, 54, "Mevcut bölümler ve EFI korunur",
                       is_dual ? COL_TEXT_PRI : COL_TEXT_DIM, 1);
        s_box_dual.x = bx + rx2; s_box_dual.y = my + 24;
        s_box_dual.w = rw2; s_box_dual.h = 56; s_box_dual.active = true;
    } else {
        surf_draw_text(s_strip, bw, bw, mh, 2, 2, "Kurulum Yöntemi:", COL_TEXT_SEC, 1);
        const char *desc1 = (scheme == INST_SCHEME_UEFI_GPT)
                            ? "Temiz Kurulum — UEFI (GPT + 64 MB FAT32 ESP + AFS v7)"
                            : "Temiz Kurulum — BIOS Legacy (MBR + AFS v7)";
        const char *desc2 = (count > 0 && sel >= 0 && sel < count && disks[sel].has_gpt)
                            ? "Mevcut GPT diskte >= 5.0 GB boş alan yok; tüm disk silinecek."
                            : "Seçili disk tamamen biçimlendirilerek Arctian OS kurulacak.";
        surf_round_rect(s_strip, bw, bw, mh, 0, 24, bw, 56, 8, COL_ROW_BG, COL_ROW_BORDER);
        surf_draw_text(s_strip, bw, bw, mh, 14, 32, desc1, COL_TEXT_PRI, 1);
        surf_draw_text(s_strip, bw, bw, mh, 14, 54, desc2, COL_TEXT_SEC, 1);
    }
    inst_blit_rect(bx, my, bw, mh, s_strip, bw);

    /* Footer Action / Confirmation / Error Bar */
    int fh = 52;
    int fy = g_lay.card_y + g_lay.card_h - fh - 14;
    surf_fill(s_strip, bw, bw, fh, COL_CARD_BG);
    s_box_rescan.active = false;
    s_box_primary.active = false;
    s_box_cancel.active = false;

    if (err_msg && err_msg[0]) {
        surf_round_rect(s_strip, bw, bw, fh, 0, 0, bw, fh, 8, COL_ERR_BG, COL_ERR);
        surf_draw_text(s_strip, bw, bw, fh, 14, 18, err_msg, COL_ERR, 1);
        int cw2 = 112;
        surf_draw_button(s_strip, bw, bw, fh, bw - cw2 - 8, 8, cw2, 36,
                         "[ESC] Tamam", COL_TEXT_PRI, COL_CARD_HDR, COL_ERR);
        s_box_cancel.x = bx + bw - cw2 - 8; s_box_cancel.y = fy + 8;
        s_box_cancel.w = cw2; s_box_cancel.h = 36; s_box_cancel.active = true;
    } else if (confirm && count > 0 && sel >= 0 && sel < count) {
        bool dual_active = (method == INST_METHOD_DUALBOOT && disks[sel].can_dualboot);
        uint32_t c_bg = dual_active ? COL_OK_BG : (disks[sel].has_gpt ? COL_ERR_BG : COL_WARN_BG);
        uint32_t c_fg = dual_active ? COL_OK    : (disks[sel].has_gpt ? COL_ERR    : COL_WARN);
        uint32_t c_brd = c_fg;
        surf_round_rect(s_strip, bw, bw, fh, 0, 0, bw, fh, 8, c_bg, c_brd);

        char msg[96];
        char *mp = msg;
        char *mend = msg + sizeof(msg);
        if (dual_active) {
            mp = inst_str_append(mp, mend, "Yanına Kur: ");
            mp = inst_str_append(mp, mend, disks[sel].info.name);
            inst_str_append(mp, mend, " (mevcut bölümler korunur)");
        } else {
            mp = inst_str_append(mp, mend, "UYARI: ");
            mp = inst_str_append(mp, mend, disks[sel].info.name);
            inst_str_append(mp, mend, " diski tamamen silinecek!");
        }
        surf_draw_text(s_strip, bw, bw, fh, 14, 18, msg, c_fg, 1);

        int pw2 = 150, cw2 = 110;
        surf_draw_button(s_strip, bw, bw, fh, bw - pw2 - 8, 8, pw2, 36,
                         "[ENTER] Onayla", COL_BG_TOP, dual_active ? COL_OK : COL_WARN,
                         dual_active ? COL_OK : COL_WARN);
        surf_draw_button(s_strip, bw, bw, fh, bw - pw2 - cw2 - 16, 8, cw2, 36,
                         "[ESC] İptal", COL_TEXT_PRI, COL_CARD_HDR, COL_ROW_BORDER);
        s_box_primary.x = bx + bw - pw2 - 8; s_box_primary.y = fy + 8;
        s_box_primary.w = pw2; s_box_primary.h = 36; s_box_primary.active = true;
        s_box_cancel.x = bx + bw - pw2 - cw2 - 16; s_box_cancel.y = fy + 8;
        s_box_cancel.w = cw2; s_box_cancel.h = 36; s_box_cancel.active = true;
    } else {
        surf_round_rect(s_strip, bw, bw, fh, 0, 0, bw, fh, 8, COL_CARD_HDR, COL_ROW_BORDER);

        int rw2 = 152;
        surf_draw_button(s_strip, bw, bw, fh, 8, 8, rw2, 36,
                         "[R] Yeniden Tara", COL_TEXT_PRI, COL_ROW_BG, COL_ROW_BORDER);
        s_box_rescan.x = bx + 8; s_box_rescan.y = fy + 8;
        s_box_rescan.w = rw2; s_box_rescan.h = 36; s_box_rescan.active = true;

        int pw2 = 190;
        surf_draw_button(s_strip, bw, bw, fh, bw - pw2 - 8, 8, pw2, 36,
                         "Kurulumu Başlat [↵]", COL_BG_TOP, COL_ACCENT, COL_ACCENT);
        s_box_primary.x = bx + bw - pw2 - 8; s_box_primary.y = fy + 8;
        s_box_primary.w = pw2; s_box_primary.h = 36; s_box_primary.active = true;

        const char *mid = "[↑/↓] Disk  [M] Mod";
        int mid_w = inst_text_width(mid, 1);
        int mid_x = rw2 + 8 + ((bw - rw2 - pw2 - 16) - mid_w) / 2;
        surf_draw_text(s_strip, bw, bw, fh, mid_x, 18, mid, COL_TEXT_SEC, 1);
    }

    inst_blit_rect(bx, fy, bw, fh, s_strip, bw);
}

static void inst_render_picker(const inst_disk_t *disks, int count, int sel, int scroll,
                               inst_scheme_t scheme, inst_method_t method,
                               bool confirm, const char *err_msg, uint64_t req_secs,
                               bool full_redraw) {
    s_ui_screen = INST_UI_SCREEN_PICKER;
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
                kprintf("  %s [%d] %s (%s)%s%s%s\n",
                        (i == sel) ? ">" : " ", i + 1, disks[i].info.name, sz, pt,
                        disks[i].can_dualboot ? " [DUALBOOT]" : "",
                        disks[i].too_small ? " [YETERSIZ]" : "");
            }
            if (confirm) {
                if (sel >= 0 && sel < count && method == INST_METHOD_DUALBOOT && disks[sel].can_dualboot) {
                    kprintf("Yanina Kur (Dual-Boot) onay: [ENTER] Onayla, [ESC] Iptal\n");
                } else {
                    kprintf("UYARI: Secili disk silinecek! [ENTER] Onayla, [ESC] Iptal\n");
                }
            }
        }
        return;
    }

    if (full_redraw) {
        inst_clear_boxes();
        inst_draw_shell("Hedef diski ve kurulum modunu seçin");
    }

    int bw = g_lay.card_w - 48;
    int bx = g_lay.card_x + 24;

    if (count <= 0) {
        int ah = 64;
        int ay = g_lay.body_y + 52;
        surf_fill(s_strip, bw, bw, ah, COL_CARD_BG);
        surf_round_rect(s_strip, bw, bw, ah, 0, 0, bw, ah, 8, COL_ERR_BG, COL_ERR);
        surf_draw_text(s_strip, bw, bw, ah, 18, 12,
                       "Kurulum için uygun yazılabilir disk bulunamadı!", COL_ERR, 1);
        surf_draw_text(s_strip, bw, bw, ah, 18, 34,
                       "Disk bağlayıp [R] tuşu ile yeniden tarayın.", COL_TEXT_PRI, 1);
        inst_blit_rect(bx, ay, bw, ah, s_strip, bw);
    } else {
        int vis = count - scroll;
        if (vis > INST_MAX_VISIBLE) vis = INST_MAX_VISIBLE;
        for (int v = 0; v < INST_MAX_VISIBLE; v++) {
            if (v < vis) {
                int idx = scroll + v;
                inst_draw_disk_row(v, idx, &disks[idx], idx == sel);
            } else {
                s_box_rows[v].active = false;
                if (full_redraw) {
                    surf_fill(s_strip, bw, bw, 48, COL_CARD_BG);
                    inst_blit_rect(bx, g_lay.body_y + 34 + v * 52, bw, 48, s_strip, bw);
                }
            }
        }
    }

    inst_draw_picker_panels(disks, count, sel, scheme, method, confirm, err_msg, req_secs);
    inst_cursor_show();
}

/* -------------------------------------------------------------------------
 * Screen 2 & 3: Real-Time Installation Progress, Success & Error UI
 * ------------------------------------------------------------------------- */
static void inst_render_progress_shell(const inst_disk_t *target,
                                       inst_scheme_t scheme, inst_method_t method) {
    s_ui_screen = INST_UI_SCREEN_PROGRESS;
    inst_clear_boxes();
    if (!g_fb.active) {
        terminal_initialize();
        show_arctian_header();
        kprintf("Kurulum baslatiliyor: %s\n", target->info.name);
        return;
    }

    inst_draw_shell("Sistem hedef diske kuruluyor");

    int bw = g_lay.card_w - 48;
    int bx = g_lay.card_x + 24;

    int th = 56;
    surf_fill(s_strip, bw, bw, th, COL_CARD_BG);
    surf_round_rect(s_strip, bw, bw, th, 0, 0, bw, th, 8, COL_ROW_BG, COL_ROW_BORDER);

    char tline[128];
    char *p = tline;
    char *end = tline + sizeof(tline);
    p = inst_str_append(p, end, "Hedef Disk: ");
    p = inst_str_append(p, end, target->info.name);
    p = inst_str_append(p, end, " (");
    char sz[24];
    inst_fmt_size(target->info.sectors, sz, sizeof(sz));
    p = inst_str_append(p, end, sz);
    p = inst_str_append(p, end, ")  |  ");
    if (method == INST_METHOD_DUALBOOT && target->can_dualboot)
        inst_str_append(p, end, "Yanına Kur (GPT Dual-Boot)");
    else if (scheme == INST_SCHEME_UEFI_GPT)
        inst_str_append(p, end, "UEFI (GPT + 64 MB ESP)");
    else
        inst_str_append(p, end, "BIOS (MBR)");
    surf_draw_text(s_strip, bw, bw, th, 16, 18, tline, COL_TEXT_PRI, 1);

    const char *bus_lbl = "ATA";
    uint32_t bus_col = COL_ATA;
    if (target->info.kind == BDEV_KIND_NVME) { bus_lbl = "NVMe"; bus_col = COL_NVME; }
    else if (target->info.kind == BDEV_KIND_AHCI) { bus_lbl = "SATA/AHCI"; bus_col = COL_SATA; }
    else if (target->info.kind == BDEV_KIND_USB) { bus_lbl = "USB"; bus_col = COL_USB; }
    int bdg_w = inst_text_width(bus_lbl, 1) + 16;
    surf_draw_badge(s_strip, bw, bw, th, bw - bdg_w - 14, 16, bus_lbl, bus_col, COL_CARD_BG, bus_col);

    inst_blit_rect(bx, g_lay.body_y + 6, bw, th, s_strip, bw);

    int fh = 52;
    int fy = g_lay.card_y + g_lay.card_h - fh - 14;
    surf_fill(s_strip, bw, bw, fh, COL_CARD_BG);
    surf_round_rect(s_strip, bw, bw, fh, 0, 0, bw, fh, 8, COL_CARD_HDR, COL_ROW_BORDER);
    const char *msg = "Kurulum sürüyor, lütfen sistemi kapatmayın...";
    int mw = inst_text_width(msg, 1);
    surf_draw_text(s_strip, bw, bw, fh, (bw - mw) / 2, 18, msg, COL_TEXT_SEC, 1);
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

    int b1h = 56;
    int b1y = g_lay.body_y + 80;
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

    int b2h = 64;
    int b2y = b1y + 64;
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

    char mb_line[64];
    char done_s[24], tot_s[24];
    inst_fmt_bytes(done_bytes, done_s, sizeof(done_s));
    inst_fmt_bytes(total_bytes ? total_bytes : 1ULL, tot_s, sizeof(tot_s));
    char *mp = mb_line;
    char *mend = mb_line + sizeof(mb_line);
    mp = inst_str_append(mp, mend, done_s);
    mp = inst_str_append(mp, mend, " / ");
    mp = inst_str_append(mp, mend, tot_s);
    inst_str_append(mp, mend, " yazıldı");
    surf_draw_text(s_strip, bw, bw, b2h, 2, 36, mb_line, COL_TEXT_SEC, 1);

    inst_blit_rect(bx, b2y, bw, b2h, s_strip, bw);
}

static void inst_render_status_card(bool is_error, const char *title, const char *subtitle,
                                    const char *footer_hint, bool show_stop_btn) {
    s_ui_screen = INST_UI_SCREEN_STATUS;
    inst_clear_boxes();
    if (!g_fb.active) {
        kprintf("\n%s: %s\n%s\n", title, subtitle, footer_hint);
        return;
    }

    int bw = g_lay.card_w - 48;
    int bx = g_lay.card_x + 24;

    int ch = 68;
    int cy = g_lay.body_y + 224;
    uint32_t fg = is_error ? COL_ERR    : COL_OK;
    uint32_t bg = is_error ? COL_ERR_BG : COL_OK_BG;

    surf_fill(s_strip, bw, bw, ch, COL_CARD_BG);
    surf_round_rect(s_strip, bw, bw, ch, 0, 0, bw, ch, 8, bg, fg);
    surf_draw_text(s_strip, bw, bw, ch, 16, 12, title, fg, 1);
    surf_draw_text(s_strip, bw, bw, ch, 16, 36, subtitle, COL_TEXT_PRI, 1);
    inst_blit_rect(bx, cy, bw, ch, s_strip, bw);

    int fh = 52;
    int fy = g_lay.card_y + g_lay.card_h - fh - 14;
    surf_fill(s_strip, bw, bw, fh, COL_CARD_BG);
    surf_round_rect(s_strip, bw, bw, fh, 0, 0, bw, fh, 8, COL_CARD_HDR, COL_ROW_BORDER);

    const char *p_lbl = is_error ? "[ENTER] Geri Dön" : "[ENTER] Yeniden Başlat";
    int pw2 = inst_text_width(p_lbl, 1) + 24;
    surf_draw_button(s_strip, bw, bw, fh, bw - pw2 - 8, 8, pw2, 36,
                     p_lbl, COL_BG_TOP, is_error ? COL_ERR : COL_OK,
                     is_error ? COL_ERR : COL_OK);
    s_box_primary.x = bx + bw - pw2 - 8; s_box_primary.y = fy + 8;
    s_box_primary.w = pw2; s_box_primary.h = 36; s_box_primary.active = true;

    int left_x = 14;
    if (show_stop_btn) {
        const char *c_lbl = "[ESC] Durdur";
        int cw2 = inst_text_width(c_lbl, 1) + 20;
        surf_draw_button(s_strip, bw, bw, fh, bw - pw2 - cw2 - 16, 8, cw2, 36,
                         c_lbl, COL_TEXT_PRI, COL_ROW_BG, COL_ROW_BORDER);
        s_box_cancel.x = bx + bw - pw2 - cw2 - 16; s_box_cancel.y = fy + 8;
        s_box_cancel.w = cw2; s_box_cancel.h = 36; s_box_cancel.active = true;
    }
    surf_draw_text(s_strip, bw, bw, fh, left_x, 18, footer_hint, COL_TEXT_SEC, 1);
    inst_blit_rect(bx, fy, bw, fh, s_strip, bw);
    inst_cursor_show();
}

/* -------------------------------------------------------------------------
 * PS/2 Keyboard + Mouse Polling (no IRQ interference while PIC is masked)
 * ------------------------------------------------------------------------- */
static void inst_kbd_flush(void) {
    s_e0_prefix = false;
    s_mouse_cycle = 0;
    for (int i = 0; i < 64; i++) {
        if (!(inb(0x64) & 0x01u)) break;
        (void)inb(0x60);
    }
}

static inst_key_t inst_handle_mouse_click(int mx, int my) {
    if (s_ui_screen == INST_UI_SCREEN_PICKER) {
        if (inst_pt_in_rect(mx, my, &s_box_mode))    return INST_KEY_MODE;
        if (inst_pt_in_rect(mx, my, &s_box_clean))   return INST_KEY_METHOD_CLEAN;
        if (inst_pt_in_rect(mx, my, &s_box_dual))    return INST_KEY_METHOD_DUAL;
        if (inst_pt_in_rect(mx, my, &s_box_rescan))  return INST_KEY_RESCAN;
        if (inst_pt_in_rect(mx, my, &s_box_primary)) return INST_KEY_ENTER;
        if (inst_pt_in_rect(mx, my, &s_box_cancel))  return INST_KEY_ESC;
        for (int v = 0; v < INST_MAX_VISIBLE; v++) {
            if (inst_pt_in_rect(mx, my, &s_box_rows[v]) && s_box_row_abs[v] >= 0) {
                s_clicked_disk_abs = s_box_row_abs[v];
                return (inst_key_t)(INST_KEY_1 + (s_clicked_disk_abs % 9));
            }
        }
    } else if (s_ui_screen == INST_UI_SCREEN_STATUS) {
        if (inst_pt_in_rect(mx, my, &s_box_primary)) return INST_KEY_ENTER;
        if (inst_pt_in_rect(mx, my, &s_box_cancel))  return INST_KEY_ESC;
    }
    return INST_KEY_NONE;
}

static inst_key_t inst_poll_key(void) {
    bool moved = false;
    while (inb(0x64) & 0x01u) {
        uint8_t st = inb(0x64);
        uint8_t sc = inb(0x60);
        if (st & 0x20u) {
            if (!s_mouse_ready) continue;
            if (s_mouse_cycle == 0) {
                if (!(sc & 0x08u)) continue; /* Wait for PS/2 sync bit */
                s_mouse_pkt[0] = sc;
                s_mouse_cycle = 1;
            } else if (s_mouse_cycle == 1) {
                s_mouse_pkt[1] = sc;
                s_mouse_cycle = 2;
            } else {
                s_mouse_pkt[2] = sc;
                s_mouse_cycle = 0;
                int dx = (int)(int8_t)s_mouse_pkt[1];
                int dy = (int)(int8_t)s_mouse_pkt[2];
                if (s_mouse_pkt[0] & 0x40u) dx = (dx > 0) ? 127 : -128;
                if (s_mouse_pkt[0] & 0x80u) dy = (dy > 0) ? 127 : -128;
                s_mouse_x += dx;
                s_mouse_y -= dy;
                if (s_mouse_x < 0) s_mouse_x = 0;
                if (s_mouse_y < 0) s_mouse_y = 0;
                if (s_mouse_x >= (int)g_fb.width)  s_mouse_x = (int)g_fb.width - 1;
                if (s_mouse_y >= (int)g_fb.height) s_mouse_y = (int)g_fb.height - 1;
                moved = true;

                bool left = (s_mouse_pkt[0] & 0x01u) != 0;
                bool clicked = left && !s_mouse_left_prev;
                s_mouse_left_prev = left;
                if (clicked) {
                    inst_cursor_show();
                    moved = false;
                    inst_key_t mk = inst_handle_mouse_click(s_mouse_x, s_mouse_y);
                    if (mk != INST_KEY_NONE) return mk;
                }
            }
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

        if (sc & 0x80u) continue;

        if (ext) {
            if (sc == 0x48u) return INST_KEY_UP;
            if (sc == 0x50u) return INST_KEY_DOWN;
            if (sc == 0x4Bu) return INST_KEY_LEFT;
            if (sc == 0x4Du) return INST_KEY_RIGHT;
            if (sc == 0x1Cu) return INST_KEY_ENTER;
            continue;
        }

        switch (sc) {
            case 0x1Cu: return INST_KEY_ENTER;
            case 0x01u: return INST_KEY_ESC;
            case 0x11u: return INST_KEY_UP;       /* W */
            case 0x1Fu: return INST_KEY_DOWN;     /* S */
            case 0x48u: return INST_KEY_UP;       /* KP 8 / Up */
            case 0x50u: return INST_KEY_DOWN;     /* KP 2 / Down */
            case 0x4Bu: return INST_KEY_LEFT;     /* KP 4 / Left */
            case 0x4Du: return INST_KEY_RIGHT;    /* KP 6 / Right */
            case 0x13u: return INST_KEY_RESCAN;   /* R */
            case 0x32u: return INST_KEY_MODE;     /* M */
            case 0x20u: return INST_KEY_DUALBOOT; /* D */
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
    if (moved) inst_cursor_show();
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
