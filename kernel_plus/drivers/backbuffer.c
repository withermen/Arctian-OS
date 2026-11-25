/**
 * Arctian Double Backbuffer
 * Off-screen framebuffer for smooth rendering
 * Dynamic size — matches current VESA mode.
 *
 * Provides double-buffering to eliminate tearing and
 * implements all UI drawing primitives (text, shapes, rounded rects).
 *
 * The backbuffer is a fixed buffer at 0x400000 sized for 2560x1440x32
 * (~14.1MB max). After drawing, backbuffer_blit() copies it to the VESA LFB.
 */
#include "backbuffer.h"
#include "vesa.h"
#include "gpu.h"
#include "gpu_simd.h"
#include "font8x8.h"
#include "kernel.h"
#include "memory_map.h"
#include "alloc.h"

/* ================================================================== */
/* Backbuffer — 16MB altinda hizli BACKBUFFER_BASE, 4K'ya (3840x2160)  */
/* kadar dinamik VMM higher-half bellekten buyur.                     */
/* ================================================================== */

/* Maximum dimensions supported (4K UHD) */
#define BB_MAX_WIDTH  3840
#define BB_MAX_HEIGHT 2160

/* Current active dimensions (set from VESA mode on init) */
static int bb_width  = 1024;
static int bb_height = 768;

static uint32_t *backbuffer = (uint32_t *)(uintptr_t)BACKBUFFER_BASE;
static size_t    backbuffer_alloc_bytes = 0;
static bool      active = false;

static void backbuffer_ensure_storage(int *w_io, int *h_io) {
    int w = *w_io;
    int h = *h_io;
    size_t need = (size_t)w * (size_t)h * 4u;
    if (need <= (size_t)BACKBUFFER_MAX) {
        if (backbuffer_alloc_bytes > 0 && backbuffer != (uint32_t *)(uintptr_t)BACKBUFFER_BASE) {
            kfree(backbuffer);
        }
        backbuffer = (uint32_t *)(uintptr_t)BACKBUFFER_BASE;
        backbuffer_alloc_bytes = 0;
        return;
    }
    if (need <= backbuffer_alloc_bytes && backbuffer) {
        return;
    }
    uint32_t *nb = (uint32_t *)kmalloc(need);
    if (nb) {
        if (backbuffer_alloc_bytes > 0 && backbuffer != (uint32_t *)(uintptr_t)BACKBUFFER_BASE) {
            kfree(backbuffer);
        }
        backbuffer = nb;
        backbuffer_alloc_bytes = need;
        gpu_simd_fill32(backbuffer, 0u, (size_t)w * (size_t)h);
        return;
    }
    /* Bellek yetmezse statik BACKBUFFER_BASE boyutuna guvenli kenetle */
    if (backbuffer_alloc_bytes > 0 && backbuffer != (uint32_t *)(uintptr_t)BACKBUFFER_BASE) {
        kfree(backbuffer);
    }
    backbuffer = (uint32_t *)(uintptr_t)BACKBUFFER_BASE;
    backbuffer_alloc_bytes = 0;
    if (w > 2560) w = 2560;
    if (h > 1440) h = 1440;
    while ((size_t)w * (size_t)h * 4u > (size_t)BACKBUFFER_MAX && h > 200) {
        h -= 16;
    }
    *w_io = w;
    *h_io = h;
}

uint32_t *backbuffer_ptr(void) {
    return backbuffer;
}

/* ================================================================== */
/* Public API                                                         */
/* ================================================================== */

int backbuffer_init(void) {
    if (!vesa_is_enabled())
        return -1;

    framebuffer_t *fb = vesa_get_framebuffer();
    if (!fb) return -1;

    /* Set dimensions from current VESA mode */
    int w = fb->width;
    int h = fb->height;

    /* Clamp to max buffer size (4K) */
    if (w > BB_MAX_WIDTH)  w = BB_MAX_WIDTH;
    if (h > BB_MAX_HEIGHT) h = BB_MAX_HEIGHT;
    backbuffer_ensure_storage(&w, &h);
    bb_width  = w;
    bb_height = h;

    /* Clear the backbuffer (not in BSS, so not automatically zeroed) */
    gpu_simd_fill32(backbuffer, 0u, (size_t)bb_width * (size_t)bb_height);

    active = true;
    return 0;
}

void backbuffer_clear(uint32_t color) {
    if (!active) return;
    gpu_simd_fill32(backbuffer, color, (size_t)bb_width * (size_t)bb_height);
}

void backbuffer_put_pixel(int x, int y, uint32_t color) {
    if (!active) return;
    if (x < 0 || x >= bb_width || y < 0 || y >= bb_height)
        return;
    backbuffer[y * bb_width + x] = color;
}

uint32_t backbuffer_get_pixel(int x, int y) {
    if (!active) return 0;
    if (x < 0 || x >= bb_width || y < 0 || y >= bb_height)
        return 0;
    return backbuffer[y * bb_width + x];
}

void backbuffer_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!active) return;

    /* Clip to screen bounds (x+w tasma tasina getirmeyen sekille) */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= bb_width || y >= bb_height) return;
    if (w > bb_width - x)  w = bb_width - x;
    if (h > bb_height - y) h = bb_height - y;
    if (w <= 0 || h <= 0) return;

    if (x == 0 && w == bb_width) {
        gpu_simd_fill32(backbuffer + (size_t)y * (size_t)bb_width,
                        color, (size_t)w * (size_t)h);
        return;
    }

    for (int row = 0; row < h; row++) {
        gpu_simd_fill32(backbuffer + (size_t)(y + row) * (size_t)bb_width + (size_t)x,
                        color, (size_t)w);
    }
}

void backbuffer_blit(void) {
    if (!active) return;

    if (gpu_is_ready()) {
        gpu_present_backbuffer(backbuffer, bb_width, bb_height, 0, 0, bb_width, bb_height);
        return;
    }

    framebuffer_t *fb = vesa_get_framebuffer();
    if (!fb || !fb->enabled || !fb->address)
        return;

    int fb_pitch_dwords = fb->pitch / 4;
    int copy_w = (bb_width < fb->width) ? bb_width : fb->width;
    int copy_h = (bb_height < fb->height) ? bb_height : fb->height;

    if (copy_w == bb_width && copy_w == fb_pitch_dwords) {
        gpu_simd_copy_wc(fb->address, backbuffer, (size_t)copy_w * (size_t)copy_h * 4u);
        return;
    }

    size_t row_bytes = (size_t)copy_w * 4u;
    for (int row = 0; row < copy_h; row++) {
        const uint32_t *s = backbuffer + (size_t)row * (size_t)bb_width;
        uint32_t *d = fb->address + (size_t)row * (size_t)fb_pitch_dwords;
        gpu_simd_copy_wc(d, s, row_bytes);
    }
}

/* Kirli bolge blit'i (performans): GPU alt sistemi (WC + SIMD + VirtIO-GPU)
 * uzerinden yalnizca degisen dikdortgeni sunar. */
void backbuffer_blit_rect(int x, int y, int w, int h) {
    if (!active || w <= 0 || h <= 0) return;

    if (gpu_is_ready()) {
        gpu_present_backbuffer(backbuffer, bb_width, bb_height, x, y, w, h);
        return;
    }

    framebuffer_t *fb = vesa_get_framebuffer();
    if (!fb || !fb->enabled || !fb->address) return;

    int fb_pitch_dwords = fb->pitch / 4;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= bb_width || y >= bb_height) return;
    if (w > bb_width - x)  w = bb_width - x;
    if (h > bb_height - y) h = bb_height - y;
    if (w > fb->width - x)  w = fb->width - x;
    if (h > fb->height - y) h = fb->height - y;
    if (w <= 0 || h <= 0) return;

    size_t row_bytes = (size_t)w * 4u;
    for (int row = y; row < y + h; row++) {
        gpu_simd_copy_wc(fb->address + (size_t)row * (size_t)fb_pitch_dwords + (size_t)x,
                         backbuffer + (size_t)row * (size_t)bb_width + (size_t)x,
                         row_bytes);
    }
}

bool backbuffer_is_active(void) {    return active;
}

int backbuffer_width(void) {
    return bb_width;
}

int backbuffer_height(void) {
    return bb_height;
}

void backbuffer_resize(int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > BB_MAX_WIDTH)  w = BB_MAX_WIDTH;
    if (h > BB_MAX_HEIGHT) h = BB_MAX_HEIGHT;
    /* LFB'dan buyuk arkatampon gercek ekrandan tasar; gercek mod
     * boyutuna da kenetle. */
    framebuffer_t *fb = vesa_get_framebuffer();
    if (fb && fb->enabled && fb->address) {
        if (fb->width > 0 && w > fb->width)  w = fb->width;
        if (fb->height > 0 && h > fb->height) h = fb->height;
    }
    backbuffer_ensure_storage(&w, &h);
    bb_width = w;
    bb_height = h;
}

/* ================================================================== */
/* Text Rendering (VGA 8x8 bitmap font)                                */
/* ================================================================== */

void backbuffer_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg) {
    if (!active) return;
    int idx = (unsigned char)c - 32;
    if (idx < 0 || idx >= 95) return;

    for (int row = 0; row < 8; row++) {
        uint8_t bits = font8x8_basic[idx][row];
        for (int col = 0; col < 8; col++) {
            uint32_t color = (bits & (0x80 >> col)) ? fg : bg;
            backbuffer_put_pixel(x + col, y + row, color);
        }
    }
}

void backbuffer_draw_string(int x, int y, const char* str, uint32_t fg, uint32_t bg) {
    if (!active || !str) return;
    while (*str) {
        backbuffer_draw_char(x, y, *str, fg, bg);
        x += 8;
        str++;
    }
}

void backbuffer_draw_string_transparent(int x, int y, const char* str, uint32_t fg) {
    if (!active || !str) return;
    while (*str) {
        int idx = (unsigned char)(*str) - 32;
        if (idx >= 0 && idx < 95) {
            for (int row = 0; row < 8; row++) {
                uint8_t bits = font8x8_basic[idx][row];
                for (int col = 0; col < 8; col++) {
                    if (bits & (0x80 >> col)) {
                        backbuffer_put_pixel(x + col, y + row, fg);
                    }
                }
            }
        }
        x += 8;
        str++;
    }
}

/* ================================================================== */
/* Smooth Font Rendering — 2x scale (16x16) per character             */
/* ================================================================== */

/**
 * Draw a single character at 1x scale (8x8 pixels) with background.
 */
void backbuffer_draw_char_scaled(int x, int y, char c, uint32_t fg, uint32_t bg) {
    if (!active) return;
    int idx = (unsigned char)c - 32;
    if (idx < 0 || idx >= 95) return;

    for (int row = 0; row < 8; row++) {
        uint8_t bits = font8x8_basic[idx][row];
        for (int col = 0; col < 8; col++) {
            uint32_t color = (bits & (0x80 >> col)) ? fg : bg;
            backbuffer_put_pixel(x + col, y + row, color);
        }
    }
}

void backbuffer_draw_string_scaled(int x, int y, const char* str, uint32_t fg, uint32_t bg) {
    if (!active || !str) return;
    while (*str) {
        backbuffer_draw_char_scaled(x, y, *str, fg, bg);
        x += 8;
        str++;
    }
}

/**
 * Draw a single character at 1x scale (8x8 pixels) with anti-aliased edges.
 * Each source pixel maps to one screen pixel.
 * Edge pixels are blended with the background for a smooth look.
 */
void backbuffer_draw_char_smooth(int x, int y, char c, uint32_t fg) {
    if (!active) return;
    int idx = (unsigned char)c - 32;
    if (idx < 0 || idx >= 95) return;

    int fr = (fg >> 16) & 0xFF;
    int fg_g = (fg >> 8) & 0xFF;
    int fb = fg & 0xFF;

    for (int row = 0; row < 8; row++) {
        uint8_t bits = font8x8_basic[idx][row];
        for (int col = 0; col < 8; col++) {
            int on = (bits & (0x80 >> col)) ? 1 : 0;
            if (!on) continue;

            /* Check neighbours for edge detection */
            int n_up    = (row > 0)   ? ((font8x8_basic[idx][row-1] & (0x80 >> col)) ? 1 : 0) : 0;
            int n_down  = (row < 7)   ? ((font8x8_basic[idx][row+1] & (0x80 >> col)) ? 1 : 0) : 0;
            int n_left  = (col > 0)   ? ((bits & (0x80 >> (col-1))) ? 1 : 0) : 0;
            int n_right = (col < 7)   ? ((bits & (0x80 >> (col+1))) ? 1 : 0) : 0;

            int is_edge = !n_up || !n_down || !n_left || !n_right;

            if (is_edge) {
                /* Count how many neighbours are off for blending weight */
                int off_count = 0;
                if (!n_up)    off_count++;
                if (!n_down)  off_count++;
                if (!n_left)  off_count++;
                if (!n_right) off_count++;

                uint32_t bg = backbuffer_get_pixel(x + col, y + row);
                int br = (bg >> 16) & 0xFF, bg_g2 = (bg >> 8) & 0xFF, bb = bg & 0xFF;

                /* Blend: more off-neighbours = more blending */
                int blend = off_count * 2; /* 2, 4, 6, or 8 */
                int total = blend + 8;
                int r = (fr * 8 + br * blend) / total;
                int g = (fg_g * 8 + bg_g2 * blend) / total;
                int b = (fb * 8 + bb * blend) / total;
                backbuffer_put_pixel(x + col, y + row, (r << 16) | (g << 8) | b);
            } else {
                backbuffer_put_pixel(x + col, y + row, fg);
            }
        }
    }
}

void backbuffer_draw_string_smooth(int x, int y, const char* str, uint32_t fg) {
    if (!active || !str) return;
    while (*str) {
        backbuffer_draw_char_smooth(x, y, *str, fg);
        x += 8;
        str++;
    }
}

/* ================================================================== */
/* Shape Helpers                                                      */
/* ================================================================== */

void backbuffer_draw_rect(int x, int y, int w, int h, uint32_t color) {
    if (!active || w <= 0 || h <= 0) return;
    backbuffer_fill_rect(x, y, w, 1, color);            /* top */
    backbuffer_fill_rect(x, y + h - 1, w, 1, color);    /* bottom */
    backbuffer_fill_rect(x, y + 1, 1, h - 2, color);    /* left */
    backbuffer_fill_rect(x + w - 1, y + 1, 1, h - 2, color); /* right */
}

/**
 * Draw a filled rectangle with anti-aliased rounded corners.
 *
 * Corner radius r is clamped to fit within half the smallest dimension.
 * Uses pixel-center distance test for smooth quarter-circle arcs
 * with 4x4 sub-pixel sampling for anti-aliasing.
 */
void backbuffer_fill_rounded_rect(int x, int y, int w, int h,
                                  int r, uint32_t color) {
    if (!active || w <= 0 || h <= 0) return;

    /* Clamp radius */
    int max_r = (w < h ? w : h) / 2;
    if (r > max_r) r = max_r;
    if (r < 1) { backbuffer_fill_rect(x, y, w, h, color); return; }

    /* Clip to screen */
    int cx1 = x, cy1 = y, cx2 = x + w, cy2 = y + h;
    if (cx2 <= 0 || cy2 <= 0 || cx1 >= bb_width || cy1 >= bb_height) return;

    /* Extract colour channels for blending */
    int base_r = (color >> 16) & 0xFF;
    int base_g = (color >> 8)  & 0xFF;
    int base_b =  color        & 0xFF;

    /* Draw each corner with anti-aliased arc */
    /* We do 4x4 sub-samples per pixel for smooth AA */
    for (int dy = 0; dy < r; dy++) {
        for (int dx = 0; dx < r; dx++) {
            /* Corner 1: top-left */
            int px = x + dx;
            int py = y + dy;
            int inside = 0;
            for (int sy = 0; sy < 4; sy++) {
                for (int sx = 0; sx < 4; sx++) {
                    int fx = (dx * 4 + sx) - (r * 4) + 2;
                    int fy = (dy * 4 + sy) - (r * 4) + 2;
                    if (fx * fx + fy * fy <= r * r * 16)
                        inside++;
                }
            }
            if (inside < 16) {
                if (inside > 0) {
                    /* Blend with background */
                    uint32_t bg = backbuffer_get_pixel(px, py);
                    int bg_a = 16 - inside;
                    int fg_a = inside;
                    int blend_r = (base_r * fg_a + ((bg >> 16) & 0xFF) * bg_a) / 16;
                    int blend_g = (base_g * fg_a + ((bg >> 8) & 0xFF) * bg_a) / 16;
                    int blend_b = (base_b * fg_a + (bg & 0xFF) * bg_a) / 16;
                    backbuffer_put_pixel(px, py, (blend_r << 16) | (blend_g << 8) | blend_b);
                }
            } else {
                backbuffer_put_pixel(px, py, color);
            }

            /* Corner 2: top-right */
            px = x + w - 1 - dx;
            py = y + dy;
            inside = 0;
            for (int sy = 0; sy < 4; sy++) {
                for (int sx = 0; sx < 4; sx++) {
                    int fx = (dx * 4 + sx) - (r * 4) + 2;
                    int fy = (dy * 4 + sy) - (r * 4) + 2;
                    if (fx * fx + fy * fy <= r * r * 16)
                        inside++;
                }
            }
            if (inside < 16) {
                if (inside > 0) {
                    uint32_t bg = backbuffer_get_pixel(px, py);
                    int bg_a = 16 - inside;
                    int fg_a = inside;
                    int blend_r = (base_r * fg_a + ((bg >> 16) & 0xFF) * bg_a) / 16;
                    int blend_g = (base_g * fg_a + ((bg >> 8) & 0xFF) * bg_a) / 16;
                    int blend_b = (base_b * fg_a + (bg & 0xFF) * bg_a) / 16;
                    backbuffer_put_pixel(px, py, (blend_r << 16) | (blend_g << 8) | blend_b);
                }
            } else {
                backbuffer_put_pixel(px, py, color);
            }

            /* Corner 3: bottom-left */
            px = x + dx;
            py = y + h - 1 - dy;
            inside = 0;
            for (int sy = 0; sy < 4; sy++) {
                for (int sx = 0; sx < 4; sx++) {
                    int fx = (dx * 4 + sx) - (r * 4) + 2;
                    int fy = (dy * 4 + sy) - (r * 4) + 2;
                    if (fx * fx + fy * fy <= r * r * 16)
                        inside++;
                }
            }
            if (inside < 16) {
                if (inside > 0) {
                    uint32_t bg = backbuffer_get_pixel(px, py);
                    int bg_a = 16 - inside;
                    int fg_a = inside;
                    int blend_r = (base_r * fg_a + ((bg >> 16) & 0xFF) * bg_a) / 16;
                    int blend_g = (base_g * fg_a + ((bg >> 8) & 0xFF) * bg_a) / 16;
                    int blend_b = (base_b * fg_a + (bg & 0xFF) * bg_a) / 16;
                    backbuffer_put_pixel(px, py, (blend_r << 16) | (blend_g << 8) | blend_b);
                }
            } else {
                backbuffer_put_pixel(px, py, color);
            }

            /* Corner 4: bottom-right */
            px = x + w - 1 - dx;
            py = y + h - 1 - dy;
            inside = 0;
            for (int sy = 0; sy < 4; sy++) {
                for (int sx = 0; sx < 4; sx++) {
                    int fx = (dx * 4 + sx) - (r * 4) + 2;
                    int fy = (dy * 4 + sy) - (r * 4) + 2;
                    if (fx * fx + fy * fy <= r * r * 16)
                        inside++;
                }
            }
            if (inside < 16) {
                if (inside > 0) {
                    uint32_t bg = backbuffer_get_pixel(px, py);
                    int bg_a = 16 - inside;
                    int fg_a = inside;
                    int blend_r = (base_r * fg_a + ((bg >> 16) & 0xFF) * bg_a) / 16;
                    int blend_g = (base_g * fg_a + ((bg >> 8) & 0xFF) * bg_a) / 16;
                    int blend_b = (base_b * fg_a + (bg & 0xFF) * bg_a) / 16;
                    backbuffer_put_pixel(px, py, (blend_r << 16) | (blend_g << 8) | blend_b);
                }
            } else {
                backbuffer_put_pixel(px, py, color);
            }
        }
    }

    /* Fill the body (everything inside the corner arcs) */
    int body_top = y + r;
    int body_bot = y + h - r;
    int body_left = x + r;
    int body_right = x + w - r;

    if (body_bot > body_top) {
        /* Full-width rows between corners */
        backbuffer_fill_rect(x, body_top, w, body_bot - body_top, color);
    }
    /* Left and right edge strips (excluding corner regions) */
    if (body_left > x) {
        backbuffer_fill_rect(x, y, body_left - x, r, color);        /* top-left edge */
        backbuffer_fill_rect(x, body_bot, body_left - x, r, color); /* bottom-left edge */
    }
    if (body_right < x + w - 1) {
        backbuffer_fill_rect(body_right + 1, y, (x + w - 1) - body_right, r, color);        /* top-right edge */
        backbuffer_fill_rect(body_right + 1, body_bot, (x + w - 1) - body_right, r, color); /* bottom-right edge */
    }
}

/**
 * Draw only the rounded border (outline) of a rounded rectangle.
 * Uses same anti-aliased corner logic but only perimeter pixels.
 */
void backbuffer_draw_rounded_rect(int x, int y, int w, int h,
                                  int r, uint32_t color) {
    if (!active || w <= 0 || h <= 0) return;

    int max_r = (w < h ? w : h) / 2;
    if (r > max_r) r = max_r;
    if (r < 1) { backbuffer_draw_rect(x, y, w, h, color); return; }

    /* Draw the four corner arcs (1px wide) */
    for (int dy = 0; dy <= r; dy++) {
        for (int dx = 0; dx <= r; dx++) {
            int dist2 = dx * dx + dy * dy;
            int r2 = r * r;
            int r1 = (r - 1) * (r - 1);
            if (r1 < 1) r1 = 0;

            if (dist2 >= r1 && dist2 <= r2) {
                backbuffer_put_pixel(x + r - dx, y + r - dy, color); /* TL */
                backbuffer_put_pixel(x + w - 1 - r + dx, y + r - dy, color); /* TR */
                backbuffer_put_pixel(x + r - dx, y + h - 1 - r + dy, color); /* BL */
                backbuffer_put_pixel(x + w - 1 - r + dx, y + h - 1 - r + dy, color); /* BR */
            }
        }
    }

    /* Straight edge segments */
    int yt = y + r;
    int yb = y + h - r - 1;
    int xl = x + r;
    int xr = x + w - r - 1;

    if (xr >= xl) {
        backbuffer_fill_rect(xl, y, xr - xl + 1, 1, color);       /* top */
        backbuffer_fill_rect(xl, y + h - 1, xr - xl + 1, 1, color); /* bottom */
    }
    if (yb >= yt) {
        backbuffer_fill_rect(x, yt, 1, yb - yt + 1, color);       /* left */
        backbuffer_fill_rect(x + w - 1, yt, 1, yb - yt + 1, color); /* right */
    }
}
