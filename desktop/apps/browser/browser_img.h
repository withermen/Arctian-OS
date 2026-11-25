/*
 * Arctian Web Engine — Freestanding Multi-Format Image Decoder
 * Zero 3rd-party dependencies, zero libc, zero libm.
 * Supports:
 *   - JPEG Baseline (SOF0) & Progressive (SOF2), 8-bit, 1/3/4 components,
 *     4:4:4 / 4:2:2 / 4:2:0 / 4:4:0 chroma subsampling, DRI restart markers.
 *   - GIF87a / GIF89a (LZW 2..12-bit, local/global palettes, transparency, interlace).
 *   - BMP (24-bit & 32-bit BI_RGB, bottom-up & top-down).
 * Output format: 0xAARRGGBB (matching LV_COLOR_FORMAT_ARGB8888 / png_decode_argb).
 */
#ifndef ARCTIAN_BROWSER_IMG_H
#define ARCTIAN_BROWSER_IMG_H

#include <stdint.h>
#include <stddef.h>

#define BIMG_MAX_DIM 4096

static inline uint8_t bimg_clamp8(int v) {
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/* ==========================================================================
 * 1. BMP Decoder (24-bit / 32-bit BI_RGB)
 * ========================================================================== */
static int bimg_decode_bmp(const uint8_t *d, uint32_t len,
                           void *(*alloc_fn)(size_t), void (*free_fn)(void *),
                           uint32_t **out_px, int *out_w, int *out_h) {
    (void)free_fn;
    if (len < 54 || d[0] != 'B' || d[1] != 'M') return -1;
    uint32_t px_off = (uint32_t)d[10] | ((uint32_t)d[11] << 8) | ((uint32_t)d[12] << 16) | ((uint32_t)d[13] << 24);
    int32_t  w      = (int32_t)((uint32_t)d[18] | ((uint32_t)d[19] << 8) | ((uint32_t)d[20] << 16) | ((uint32_t)d[21] << 24));
    int32_t  h_raw  = (int32_t)((uint32_t)d[22] | ((uint32_t)d[23] << 8) | ((uint32_t)d[24] << 16) | ((uint32_t)d[25] << 24));
    uint16_t bpp    = (uint16_t)(d[28] | ((uint16_t)d[29] << 8));
    uint32_t comp   = (uint32_t)d[30] | ((uint32_t)d[31] << 8) | ((uint32_t)d[32] << 16) | ((uint32_t)d[33] << 24);
    int top_down = 0;
    int h = h_raw;
    if (h < 0) { h = -h; top_down = 1; }
    if (w <= 0 || h <= 0 || w > BIMG_MAX_DIM || h > BIMG_MAX_DIM) return -2;
    if (comp != 0 || (bpp != 24 && bpp != 32)) return -3;
    uint32_t row_bytes = (bpp == 24) ? (((uint32_t)w * 3u + 3u) & ~3u) : ((uint32_t)w * 4u);
    if (px_off > len || (uint64_t)px_off + (uint64_t)row_bytes * (uint64_t)h > len) return -4;

    uint32_t *px = (uint32_t *)alloc_fn((size_t)w * (size_t)h * 4u);
    if (!px) return -5;
    for (int y = 0; y < h; y++) {
        int dy = top_down ? y : (h - 1 - y);
        const uint8_t *src = d + px_off + (uint32_t)y * row_bytes;
        uint32_t *dst = px + (size_t)dy * (size_t)w;
        if (bpp == 24) {
            for (int x = 0; x < w; x++) {
                uint8_t b = src[x * 3 + 0], g = src[x * 3 + 1], r = src[x * 3 + 2];
                dst[x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
        } else {
            for (int x = 0; x < w; x++) {
                uint8_t b = src[x * 4 + 0], g = src[x * 4 + 1], r = src[x * 4 + 2], a = src[x * 4 + 3];
                if (a == 0) a = 0xFF;
                dst[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
        }
    }
    *out_px = px; *out_w = w; *out_h = h;
    return 0;
}

/* ==========================================================================
 * 2. GIF87a / GIF89a Decoder (LZW + Transparency + Interlace)
 * ========================================================================== */
static int bimg_decode_gif(const uint8_t *d, uint32_t len,
                           void *(*alloc_fn)(size_t), void (*free_fn)(void *),
                           uint32_t **out_px, int *out_w, int *out_h) {
    if (len < 14 || d[0] != 'G' || d[1] != 'I' || d[2] != 'F' || d[3] != '8') return -1;
    int sw = (int)(d[6] | ((uint16_t)d[7] << 8));
    int sh = (int)(d[8] | ((uint16_t)d[9] << 8));
    if (sw <= 0 || sh <= 0 || sw > BIMG_MAX_DIM || sh > BIMG_MAX_DIM) return -2;

    uint8_t flags = d[10];
    uint8_t bg_idx = d[11];
    uint32_t p = 13;
    const uint8_t *gct = 0;
    int gct_n = 0;
    if (flags & 0x80) {
        gct_n = 1 << ((flags & 7) + 1);
        if (p + (uint32_t)gct_n * 3u > len) return -3;
        gct = d + p;
        p += (uint32_t)gct_n * 3u;
    }

    int has_trans = 0, trans_idx = -1;
    while (p < len) {
        uint8_t tag = d[p++];
        if (tag == 0x3B) return -4; /* trailer before any image */
        if (tag == 0x21) { /* extension */
            if (p >= len) return -4;
            uint8_t label = d[p++];
            if (label == 0xF9 && p + 5 < len && d[p] == 4) {
                uint8_t gce_f = d[p + 1];
                has_trans = (gce_f & 1);
                trans_idx = d[p + 4];
            }
            while (p < len) {
                uint8_t bsz = d[p++];
                if (bsz == 0) break;
                if (p + bsz > len) return -4;
                p += bsz;
            }
            continue;
        }
        if (tag != 0x2C) continue;
        if (p + 9 > len) return -5;
        int ix = (int)(d[p] | ((uint16_t)d[p + 1] << 8));
        int iy = (int)(d[p + 2] | ((uint16_t)d[p + 3] << 8));
        int iw = (int)(d[p + 4] | ((uint16_t)d[p + 5] << 8));
        int ih = (int)(d[p + 6] | ((uint16_t)d[p + 7] << 8));
        uint8_t iflags = d[p + 8];
        p += 9;
        if (iw <= 0 || ih <= 0 || iw > BIMG_MAX_DIM || ih > BIMG_MAX_DIM) return -5;

        const uint8_t *pal = gct;
        int pal_n = gct_n;
        if (iflags & 0x80) {
            pal_n = 1 << ((iflags & 7) + 1);
            if (p + (uint32_t)pal_n * 3u > len) return -5;
            pal = d + p;
            p += (uint32_t)pal_n * 3u;
        }
        if (!pal || p >= len) return -5;
        int interlace = (iflags & 0x40) ? 1 : 0;
        int min_cs = d[p++];
        if (min_cs < 2 || min_cs > 11) return -6;

        /* Gather LZW sub-blocks into contiguous buffer */
        uint32_t q = p, lzw_total = 0;
        while (q < len) {
            uint8_t bsz = d[q++];
            if (bsz == 0) break;
            if (q + bsz > len) { bsz = (uint8_t)(len - q); }
            lzw_total += bsz;
            q += bsz;
        }
        if (lzw_total == 0) return -6;
        uint8_t *lzw_buf = (uint8_t *)alloc_fn(lzw_total + 4u);
        if (!lzw_buf) return -7;
        uint32_t lo = 0;
        while (p < len) {
            uint8_t bsz = d[p++];
            if (bsz == 0) break;
            if (p + bsz > len) bsz = (uint8_t)(len - p);
            for (uint32_t k = 0; k < bsz; k++) lzw_buf[lo++] = d[p + k];
            p += bsz;
        }
        lzw_buf[lo] = 0; lzw_buf[lo + 1] = 0; lzw_buf[lo + 2] = 0; lzw_buf[lo + 3] = 0;

        /* LZW tables on heap (avoids stack bloat in freestanding kernel/desktop) */
        typedef struct {
            int16_t prefix[4096];
            uint8_t suffix[4096];
            uint8_t stack[4096];
        } gif_lzw_t;
        gif_lzw_t *st = (gif_lzw_t *)alloc_fn(sizeof(gif_lzw_t));
        uint8_t *idx_out = (uint8_t *)alloc_fn((size_t)iw * (size_t)ih);
        if (!st || !idx_out) {
            if (st) free_fn(st);
            if (idx_out) free_fn(idx_out);
            free_fn(lzw_buf);
            return -7;
        }

        int clear_code = 1 << min_cs;
        int eoi_code   = clear_code + 1;
        int next_code  = eoi_code + 1;
        int code_size  = min_cs + 1;
        int old_code   = -1;
        int first_char = 0;
        uint32_t bit_pos = 0, bit_total = lzw_total * 8u;
        size_t out_n = 0, out_max = (size_t)iw * (size_t)ih;

        for (int i = 0; i < clear_code; i++) { st->prefix[i] = -1; st->suffix[i] = (uint8_t)i; }

        while (bit_pos + (uint32_t)code_size <= bit_total && out_n < out_max) {
            uint32_t byte_i = bit_pos >> 3;
            uint32_t bit_o  = bit_pos & 7u;
            uint32_t raw = (uint32_t)lzw_buf[byte_i] |
                           ((uint32_t)lzw_buf[byte_i + 1] << 8) |
                           ((uint32_t)lzw_buf[byte_i + 2] << 16);
            int code = (int)((raw >> bit_o) & ((1u << code_size) - 1u));
            bit_pos += (uint32_t)code_size;

            if (code == clear_code) {
                next_code = eoi_code + 1;
                code_size = min_cs + 1;
                old_code  = -1;
                continue;
            }
            if (code == eoi_code) break;

            int in_code = code;
            int sp = 0;
            if (code >= next_code) {
                if (old_code < 0 || code > next_code) break;
                st->stack[sp++] = (uint8_t)first_char;
                code = old_code;
            }
            while (code >= clear_code && sp < 4095) {
                st->stack[sp++] = st->suffix[code];
                code = st->prefix[code];
                if (code < 0) break;
            }
            if (code < 0 || code >= 4096) break;
            first_char = st->suffix[code];
            st->stack[sp++] = (uint8_t)first_char;

            while (sp > 0 && out_n < out_max) idx_out[out_n++] = st->stack[--sp];

            if (old_code >= 0 && next_code < 4096) {
                st->prefix[next_code] = (int16_t)old_code;
                st->suffix[next_code] = (uint8_t)first_char;
                next_code++;
                if (next_code == (1 << code_size) && code_size < 12) code_size++;
            }
            old_code = in_code;
        }

        uint32_t *px = (uint32_t *)alloc_fn((size_t)sw * (size_t)sh * 4u);
        if (!px) { free_fn(st); free_fn(idx_out); free_fn(lzw_buf); return -7; }
        uint32_t bg_c = 0x00000000u;
        if (!has_trans && gct && bg_idx < gct_n) {
            bg_c = 0xFF000000u | ((uint32_t)gct[bg_idx * 3] << 16) |
                   ((uint32_t)gct[bg_idx * 3 + 1] << 8) | (uint32_t)gct[bg_idx * 3 + 2];
        }
        for (size_t i = 0; i < (size_t)sw * (size_t)sh; i++) px[i] = bg_c;

        static const int il_start[4] = { 0, 4, 2, 1 };
        static const int il_step[4]  = { 8, 8, 4, 2 };
        int src_y = 0;
        if (interlace) {
            for (int pass = 0; pass < 4; pass++) {
                for (int y = il_start[pass]; y < ih; y += il_step[pass]) {
                    if (src_y >= ih) break;
                    int dy = iy + y;
                    if (dy >= 0 && dy < sh) {
                        for (int x = 0; x < iw; x++) {
                            int dx = ix + x;
                            if (dx < 0 || dx >= sw) continue;
                            uint8_t ci = idx_out[(size_t)src_y * (size_t)iw + (size_t)x];
                            if (has_trans && (int)ci == trans_idx) continue;
                            if (ci < pal_n) {
                                px[(size_t)dy * (size_t)sw + (size_t)dx] =
                                    0xFF000000u | ((uint32_t)pal[ci * 3] << 16) |
                                    ((uint32_t)pal[ci * 3 + 1] << 8) | (uint32_t)pal[ci * 3 + 2];
                            }
                        }
                    }
                    src_y++;
                }
            }
        } else {
            for (int y = 0; y < ih; y++) {
                int dy = iy + y;
                if (dy < 0 || dy >= sh) continue;
                for (int x = 0; x < iw; x++) {
                    int dx = ix + x;
                    if (dx < 0 || dx >= sw) continue;
                    uint8_t ci = idx_out[(size_t)y * (size_t)iw + (size_t)x];
                    if (has_trans && (int)ci == trans_idx) continue;
                    if (ci < pal_n) {
                        px[(size_t)dy * (size_t)sw + (size_t)dx] =
                            0xFF000000u | ((uint32_t)pal[ci * 3] << 16) |
                            ((uint32_t)pal[ci * 3 + 1] << 8) | (uint32_t)pal[ci * 3 + 2];
                    }
                }
            }
        }

        free_fn(st);
        free_fn(idx_out);
        free_fn(lzw_buf);
        *out_px = px; *out_w = sw; *out_h = sh;
        return 0;
    }
    return -8;
}

/* ==========================================================================
 * 3. JPEG Decoder — Baseline (SOF0) & Progressive (SOF2)
 *    Pure integer IDCT, Huffman, DRI restart, 4:4:4 / 4:2:2 / 4:2:0 / gray.
 * ========================================================================== */
static const uint8_t BJPG_ZZ[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

typedef struct {
    uint8_t  fast_sym[256];
    uint8_t  fast_len[256];
    uint16_t max_code[18];
    int32_t  val_off[18];
    uint8_t  values[256];
    int      valid;
} bjpg_huff_t;

typedef struct {
    int id, hs, vs, tq;
    int td, ta;
    int dc_pred;
    int bw, bh;      /* width & height in 8x8 blocks */
    int16_t *coeffs; /* used for progressive mode */
    uint8_t *plane;  /* decoded sample plane (bw*8 x bh*8) */
} bjpg_comp_t;

typedef struct {
    const uint8_t *d;
    uint32_t len, pos;
    uint32_t bit_buf;
    int      bit_cnt;
    int      eof_marker;

    int w, h, ncomp, progressive;
    int max_hs, max_vs;
    int restart_interval;
    int eobrun;

    uint16_t    qt[4][64];
    int         qt_valid[4];
    bjpg_huff_t huff_dc[4];
    bjpg_huff_t huff_ac[4];
    bjpg_comp_t comp[4];

    void *(*alloc_fn)(size_t);
    void  (*free_fn)(void *);
} bjpg_t;

static void bjpg_build_huff(bjpg_huff_t *h, const uint8_t counts[16], const uint8_t *vals) {
    for (int i = 0; i < 256; i++) { h->fast_sym[i] = 0; h->fast_len[i] = 0; }
    int total = 0;
    for (int i = 0; i < 16; i++) total += counts[i];
    if (total > 256) total = 256;
    for (int i = 0; i < total; i++) h->values[i] = vals[i];

    int code = 0, k = 0;
    for (int bits = 1; bits <= 16; bits++) {
        h->val_off[bits] = k - code;
        for (int j = 0; j < counts[bits - 1] && k < total; j++) {
            if (bits <= 8) {
                int shift = 8 - bits;
                int base = code << shift;
                int span = 1 << shift;
                for (int m = 0; m < span; m++) {
                    h->fast_sym[base + m] = h->values[k];
                    h->fast_len[base + m] = (uint8_t)bits;
                }
            }
            code++;
            k++;
        }
        h->max_code[bits] = (uint16_t)code; /* exclusive upper bound */
        code <<= 1;
    }
    h->valid = 1;
}

static inline void bjpg_fill_bits(bjpg_t *j) {
    while (j->bit_cnt <= 24) {
        if (j->pos >= j->len) {
            j->bit_buf = (j->bit_buf << 8);
            j->bit_cnt += 8;
            continue;
        }
        uint8_t b = j->d[j->pos++];
        if (b == 0xFF) {
            while (j->pos < j->len && j->d[j->pos] == 0xFF) j->pos++;
            if (j->pos < j->len) {
                uint8_t nxt = j->d[j->pos++];
                if (nxt == 0x00) {
                    /* byte-stuffed 0xFF */
                } else {
                    /* encountered a marker inside scan */
                    j->pos -= 2;
                    j->eof_marker = nxt;
                    b = 0;
                }
            }
        }
        j->bit_buf = (j->bit_buf << 8) | (uint32_t)b;
        j->bit_cnt += 8;
    }
}

static inline uint32_t bjpg_peek_bits(bjpg_t *j, int n) {
    if (j->bit_cnt < n) bjpg_fill_bits(j);
    return (j->bit_buf >> (j->bit_cnt - n)) & ((1u << n) - 1u);
}

static inline uint32_t bjpg_get_bits(bjpg_t *j, int n) {
    if (n == 0) return 0;
    if (j->bit_cnt < n) bjpg_fill_bits(j);
    j->bit_cnt -= n;
    return (j->bit_buf >> j->bit_cnt) & ((1u << n) - 1u);
}

static inline int bjpg_extend(int v, int bits) {
    if (bits <= 0) return 0;
    int vt = 1 << (bits - 1);
    return (v < vt) ? (v - ((1 << bits) - 1)) : v;
}

static int bjpg_huff_dec(bjpg_t *j, const bjpg_huff_t *h) {
    if (!h->valid) return -1;
    if (j->bit_cnt < 16) bjpg_fill_bits(j);
    uint32_t top8 = (j->bit_buf >> (j->bit_cnt - 8)) & 0xFFu;
    uint8_t fl = h->fast_len[top8];
    if (fl) {
        j->bit_cnt -= fl;
        return h->fast_sym[top8];
    }
    int code = (int)bjpg_get_bits(j, 8);
    for (int bits = 9; bits <= 16; bits++) {
        code = (code << 1) | (int)bjpg_get_bits(j, 1);
        if (code < (int)h->max_code[bits]) {
            int idx = code + h->val_off[bits];
            if (idx >= 0 && idx < 256) return h->values[idx];
            return -1;
        }
    }
    return -1;
}

/* Exact fixed-point 8x8 2D IDCT (Loeffler/IJG scaled, 12-bit precision) */
static void bjpg_idct_8x8(const int16_t coeff[64], const uint16_t qt[64], uint8_t *out, int stride) {
    int tmp[64];
    /* Pass 1: columns */
    for (int i = 0; i < 8; i++) {
        int s0 = coeff[i +  0] * qt[i +  0];
        int s1 = coeff[i +  8] * qt[i +  8];
        int s2 = coeff[i + 16] * qt[i + 16];
        int s3 = coeff[i + 24] * qt[i + 24];
        int s4 = coeff[i + 32] * qt[i + 32];
        int s5 = coeff[i + 40] * qt[i + 40];
        int s6 = coeff[i + 48] * qt[i + 48];
        int s7 = coeff[i + 56] * qt[i + 56];

        if ((s1 | s2 | s3 | s4 | s5 | s6 | s7) == 0) {
            int dc = s0 << 2;
            tmp[i +  0] = dc; tmp[i +  8] = dc; tmp[i + 16] = dc; tmp[i + 24] = dc;
            tmp[i + 32] = dc; tmp[i + 40] = dc; tmp[i + 48] = dc; tmp[i + 56] = dc;
            continue;
        }

        int p2 = s2, p3 = s6;
        int p1 = (p2 + p3) * 2217;
        int t2 = p1 + p3 * (-7567);
        int t3 = p1 + p2 * 3135;
        int t0 = (s0 + s4) << 12;
        int t1 = (s0 - s4) << 12;
        int x0 = t0 + t3 + 512;
        int x3 = t0 - t3 + 512;
        int x1 = t1 + t2 + 512;
        int x2 = t1 - t2 + 512;

        t0 = s7; t1 = s5; t2 = s3; t3 = s1;
        p3 = t0 + t2;
        int p4 = t1 + t3;
        p1 = t0 + t3;
        p2 = t1 + t2;
        int p5 = (p3 + p4) * 4816;

        t0 = t0 * 1223; t1 = t1 * 8410; t2 = t2 * 12586; t3 = t3 * 6149;
        p1 = p5 + p1 * (-3685);
        p2 = p5 + p2 * (-10497);
        p3 = p3 * (-8034);
        p4 = p4 * (-1597);

        t3 += p1 + p4;
        t2 += p2 + p3;
        t1 += p2 + p4;
        t0 += p1 + p3;

        tmp[i +  0] = (x0 + t3) >> 10;
        tmp[i +  8] = (x1 + t2) >> 10;
        tmp[i + 16] = (x2 + t1) >> 10;
        tmp[i + 24] = (x3 + t0) >> 10;
        tmp[i + 32] = (x3 - t0) >> 10;
        tmp[i + 40] = (x2 - t1) >> 10;
        tmp[i + 48] = (x1 - t2) >> 10;
        tmp[i + 56] = (x0 - t3) >> 10;
    }

    /* Pass 2: rows */
    for (int i = 0; i < 8; i++) {
        int *r = tmp + i * 8;
        uint8_t *dst = out + i * stride;
        int s0 = r[0], s1 = r[1], s2 = r[2], s3 = r[3], s4 = r[4], s5 = r[5], s6 = r[6], s7 = r[7];
        if ((s1 | s2 | s3 | s4 | s5 | s6 | s7) == 0) {
            uint8_t v = bimg_clamp8(((s0 + 16) >> 5) + 128);
            dst[0] = v; dst[1] = v; dst[2] = v; dst[3] = v;
            dst[4] = v; dst[5] = v; dst[6] = v; dst[7] = v;
            continue;
        }
        int p2 = s2, p3 = s6;
        int p1 = (p2 + p3) * 2217;
        int t2 = p1 + p3 * (-7567);
        int t3 = p1 + p2 * 3135;
        int t0 = (s0 + s4) << 12;
        int t1 = (s0 - s4) << 12;
        int x0 = t0 + t3 + 65536 + (128 << 17);
        int x3 = t0 - t3 + 65536 + (128 << 17);
        int x1 = t1 + t2 + 65536 + (128 << 17);
        int x2 = t1 - t2 + 65536 + (128 << 17);

        t0 = s7; t1 = s5; t2 = s3; t3 = s1;
        p3 = t0 + t2;
        int p4 = t1 + t3;
        p1 = t0 + t3;
        p2 = t1 + t2;
        int p5 = (p3 + p4) * 4816;

        t0 = t0 * 1223; t1 = t1 * 8410; t2 = t2 * 12586; t3 = t3 * 6149;
        p1 = p5 + p1 * (-3685);
        p2 = p5 + p2 * (-10497);
        p3 = p3 * (-8034);
        p4 = p4 * (-1597);

        t3 += p1 + p4;
        t2 += p2 + p3;
        t1 += p2 + p4;
        t0 += p1 + p3;

        dst[0] = bimg_clamp8((x0 + t3) >> 17);
        dst[1] = bimg_clamp8((x1 + t2) >> 17);
        dst[2] = bimg_clamp8((x2 + t1) >> 17);
        dst[3] = bimg_clamp8((x3 + t0) >> 17);
        dst[4] = bimg_clamp8((x3 - t0) >> 17);
        dst[5] = bimg_clamp8((x2 - t1) >> 17);
        dst[6] = bimg_clamp8((x1 - t2) >> 17);
        dst[7] = bimg_clamp8((x0 - t3) >> 17);
    }
}

static int bjpg_decode_block_baseline(bjpg_t *j, bjpg_comp_t *c, int16_t out[64]) {
    for (int i = 0; i < 64; i++) out[i] = 0;
    int s = bjpg_huff_dec(j, &j->huff_dc[c->td]);
    if (s < 0 || s > 15) return -1;
    int diff = s ? bjpg_extend((int)bjpg_get_bits(j, s), s) : 0;
    c->dc_pred += diff;
    out[0] = (int16_t)c->dc_pred;

    int k = 1;
    while (k < 64) {
        int rs = bjpg_huff_dec(j, &j->huff_ac[c->ta]);
        if (rs < 0) return -1;
        int r = rs >> 4;
        int sz = rs & 15;
        if (sz == 0) {
            if (r == 15) { k += 16; continue; }
            break; /* EOB */
        }
        k += r;
        if (k >= 64) break;
        int v = bjpg_extend((int)bjpg_get_bits(j, sz), sz);
        out[BJPG_ZZ[k]] = (int16_t)v;
        k++;
    }
    return 0;
}

static int bjpg_decode_block_prog(bjpg_t *j, bjpg_comp_t *c, int16_t *blk,
                                  int ss, int se, int ah, int al) {
    if (ss == 0) {
        if (ah == 0) {
            int s = bjpg_huff_dec(j, &j->huff_dc[c->td]);
            if (s < 0 || s > 15) return -1;
            int diff = s ? bjpg_extend((int)bjpg_get_bits(j, s), s) : 0;
            c->dc_pred += diff;
            blk[0] = (int16_t)(c->dc_pred << al);
        } else {
            if (bjpg_get_bits(j, 1)) blk[0] = (int16_t)(blk[0] | (1 << al));
        }
        return 0;
    }
    /* AC scan */
    if (ah == 0) {
        if (j->eobrun > 0) { j->eobrun--; return 0; }
        int k = ss;
        while (k <= se) {
            int rs = bjpg_huff_dec(j, &j->huff_ac[c->ta]);
            if (rs < 0) return -1;
            int r = rs >> 4;
            int sz = rs & 15;
            if (sz == 0) {
                if (r == 15) { k += 16; continue; }
                j->eobrun = (1 << r) + (r ? (int)bjpg_get_bits(j, r) : 0) - 1;
                break;
            }
            k += r;
            if (k > se) break;
            int v = bjpg_extend((int)bjpg_get_bits(j, sz), sz);
            blk[BJPG_ZZ[k]] = (int16_t)(v << al);
            k++;
        }
    } else {
        /* AC refinement scan */
        int p1 = 1 << al, m1 = -(1 << al);
        int k = ss;
        if (j->eobrun == 0) {
            while (k <= se) {
                int rs = bjpg_huff_dec(j, &j->huff_ac[c->ta]);
                if (rs < 0) return -1;
                int r = rs >> 4;
                int sz = rs & 15;
                int v = 0;
                if (sz) {
                    v = bjpg_get_bits(j, 1) ? p1 : m1;
                } else if (r != 15) {
                    j->eobrun = (1 << r) + (r ? (int)bjpg_get_bits(j, r) : 0);
                    break;
                }
                while (k <= se) {
                    int16_t *p = &blk[BJPG_ZZ[k]];
                    if (*p != 0) {
                        if (bjpg_get_bits(j, 1)) {
                            if ((*p & p1) == 0) *p = (int16_t)(*p + (*p > 0 ? p1 : m1));
                        }
                    } else {
                        if (r == 0) { *p = (int16_t)v; k++; break; }
                        r--;
                    }
                    k++;
                }
            }
        }
        if (j->eobrun > 0) {
            for (; k <= se; k++) {
                int16_t *p = &blk[BJPG_ZZ[k]];
                if (*p != 0 && bjpg_get_bits(j, 1)) {
                    if ((*p & p1) == 0) *p = (int16_t)(*p + (*p > 0 ? p1 : m1));
                }
            }
            j->eobrun--;
        }
    }
    return 0;
}

static void bjpg_handle_restart(bjpg_t *j, int *todo) {
    if (j->restart_interval <= 0) return;
    (*todo)--;
    if (*todo > 0) return;
    *todo = j->restart_interval;
    j->bit_cnt = 0;
    j->bit_buf = 0;
    j->eobrun = 0;
    for (int i = 0; i < j->ncomp; i++) j->comp[i].dc_pred = 0;
    if (j->eof_marker >= 0xD0 && j->eof_marker <= 0xD7) {
        if (j->pos + 2 <= j->len && j->d[j->pos] == 0xFF && j->d[j->pos + 1] == (uint8_t)j->eof_marker)
            j->pos += 2;
        j->eof_marker = 0;
    } else {
        while (j->pos + 1 < j->len) {
            if (j->d[j->pos] == 0xFF && j->d[j->pos + 1] >= 0xD0 && j->d[j->pos + 1] <= 0xD7) {
                j->pos += 2;
                break;
            }
            if (j->d[j->pos] == 0xFF && j->d[j->pos + 1] != 0x00 && j->d[j->pos + 1] != 0xFF) break;
            j->pos++;
        }
    }
}

static int bimg_decode_jpeg(const uint8_t *d, uint32_t len,
                            void *(*alloc_fn)(size_t), void (*free_fn)(void *),
                            uint32_t **out_px, int *out_w, int *out_h) {
    if (len < 4 || d[0] != 0xFF || d[1] != 0xD8) return -1;
    bjpg_t *j = (bjpg_t *)alloc_fn(sizeof(bjpg_t));
    if (!j) return -2;
    { uint8_t *z = (uint8_t *)j; for (size_t i = 0; i < sizeof(bjpg_t); i++) z[i] = 0; }
    j->d = d; j->len = len; j->pos = 2;
    j->alloc_fn = alloc_fn; j->free_fn = free_fn;

    int err = 0, got_sof = 0, got_sos = 0;
    while (j->pos + 1 < j->len && !err) {
        while (j->pos < j->len && j->d[j->pos] != 0xFF) j->pos++;
        while (j->pos < j->len && j->d[j->pos] == 0xFF) j->pos++;
        if (j->pos >= j->len) break;
        uint8_t m = j->d[j->pos++];
        if (m == 0xD9) break; /* EOI */
        if (m == 0x00 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) continue;
        if (j->pos + 2 > j->len) break;
        uint32_t seg_len = ((uint32_t)j->d[j->pos] << 8) | j->d[j->pos + 1];
        if (seg_len < 2 || j->pos + seg_len > j->len) { err = -3; break; }
        const uint8_t *sp = j->d + j->pos + 2;
        uint32_t sl = seg_len - 2;
        j->pos += seg_len;

        if (m == 0xDB) { /* DQT */
            uint32_t o = 0;
            while (o < sl) {
                uint8_t pq_tq = sp[o++];
                int pq = pq_tq >> 4, tq = pq_tq & 15;
                if (tq > 3) { err = -4; break; }
                uint32_t need = pq ? 128u : 64u;
                if (o + need > sl) { err = -4; break; }
                for (int k = 0; k < 64; k++) {
                    uint16_t v = pq ? (uint16_t)(((uint16_t)sp[o + k * 2] << 8) | sp[o + k * 2 + 1]) : sp[o + k];
                    j->qt[tq][BJPG_ZZ[k]] = v ? v : 1;
                }
                j->qt_valid[tq] = 1;
                o += need;
            }
        } else if (m == 0xC4) { /* DHT */
            uint32_t o = 0;
            while (o + 17 <= sl) {
                uint8_t tc_th = sp[o++];
                int tc = tc_th >> 4, th = tc_th & 15;
                if (tc > 1 || th > 3) { err = -5; break; }
                const uint8_t *counts = sp + o; o += 16;
                int sum = 0;
                for (int k = 0; k < 16; k++) sum += counts[k];
                if (o + (uint32_t)sum > sl) { err = -5; break; }
                bjpg_build_huff(tc ? &j->huff_ac[th] : &j->huff_dc[th], counts, sp + o);
                o += (uint32_t)sum;
            }
        } else if (m == 0xDD) { /* DRI */
            if (sl >= 2) j->restart_interval = ((int)sp[0] << 8) | sp[1];
        } else if (m == 0xC0 || m == 0xC1 || m == 0xC2) { /* SOF0 / SOF1 / SOF2 */
            if (sl < 6) { err = -6; break; }
            int prec = sp[0];
            if (prec != 8) { err = -6; break; }
            j->progressive = (m == 0xC2) ? 1 : 0;
            j->h = ((int)sp[1] << 8) | sp[2];
            j->w = ((int)sp[3] << 8) | sp[4];
            j->ncomp = sp[5];
            if (j->w <= 0 || j->h <= 0 || j->w > BIMG_MAX_DIM || j->h > BIMG_MAX_DIM) { err = -7; break; }
            if (j->ncomp != 1 && j->ncomp != 3 && j->ncomp != 4) { err = -7; break; }
            if (sl < (uint32_t)(6 + j->ncomp * 3)) { err = -7; break; }
            j->max_hs = 1; j->max_vs = 1;
            for (int i = 0; i < j->ncomp; i++) {
                j->comp[i].id = sp[6 + i * 3];
                j->comp[i].hs = sp[6 + i * 3 + 1] >> 4;
                j->comp[i].vs = sp[6 + i * 3 + 1] & 15;
                j->comp[i].tq = sp[6 + i * 3 + 2] & 3;
                if (j->comp[i].hs < 1 || j->comp[i].hs > 4 || j->comp[i].vs < 1 || j->comp[i].vs > 4) { err = -7; break; }
                if (j->comp[i].hs > j->max_hs) j->max_hs = j->comp[i].hs;
                if (j->comp[i].vs > j->max_vs) j->max_vs = j->comp[i].vs;
            }
            if (err) break;
            int mcu_w = j->max_hs * 8;
            int mcu_h = j->max_vs * 8;
            int mcus_x = (j->w + mcu_w - 1) / mcu_w;
            int mcus_y = (j->h + mcu_h - 1) / mcu_h;
            for (int i = 0; i < j->ncomp; i++) {
                j->comp[i].bw = mcus_x * j->comp[i].hs;
                j->comp[i].bh = mcus_y * j->comp[i].vs;
                size_t psz = (size_t)j->comp[i].bw * 8u * (size_t)j->comp[i].bh * 8u;
                j->comp[i].plane = (uint8_t *)alloc_fn(psz);
                if (!j->comp[i].plane) { err = -8; break; }
                if (j->progressive) {
                    size_t csz = (size_t)j->comp[i].bw * (size_t)j->comp[i].bh * 64u * sizeof(int16_t);
                    j->comp[i].coeffs = (int16_t *)alloc_fn(csz);
                    if (!j->comp[i].coeffs) { err = -8; break; }
                    uint8_t *cz = (uint8_t *)j->comp[i].coeffs;
                    for (size_t z = 0; z < csz; z++) cz[z] = 0;
                }
            }
            got_sof = 1;
        } else if (m == 0xDA) { /* SOS */
            if (!got_sof || sl < 4) { err = -9; break; }
            int ns = sp[0];
            if (ns < 1 || ns > j->ncomp || sl < (uint32_t)(1 + ns * 2 + 3)) { err = -9; break; }
            int sc_idx[4];
            for (int i = 0; i < ns; i++) {
                int cid = sp[1 + i * 2];
                int td_ta = sp[1 + i * 2 + 1];
                int ci = -1;
                for (int c = 0; c < j->ncomp; c++) if (j->comp[c].id == cid) { ci = c; break; }
                if (ci < 0) ci = i < j->ncomp ? i : 0;
                sc_idx[i] = ci;
                j->comp[ci].td = (td_ta >> 4) & 3;
                j->comp[ci].ta = td_ta & 3;
                j->comp[ci].dc_pred = 0;
            }
            int ss = sp[1 + ns * 2];
            int se = sp[1 + ns * 2 + 1];
            int ah = sp[1 + ns * 2 + 2] >> 4;
            int al = sp[1 + ns * 2 + 2] & 15;
            if (se > 63) se = 63;

            j->bit_buf = 0; j->bit_cnt = 0; j->eof_marker = 0; j->eobrun = 0;
            int todo = j->restart_interval;

            if (!j->progressive) {
                int mcu_w = j->max_hs * 8, mcu_h = j->max_vs * 8;
                int mcus_x = (j->w + mcu_w - 1) / mcu_w;
                int mcus_y = (j->h + mcu_h - 1) / mcu_h;
                int16_t blk[64];
                if (ns == 1) {
                    bjpg_comp_t *c = &j->comp[sc_idx[0]];
                    int bx_n = (j->w * c->hs + j->max_hs * 8 - 1) / (j->max_hs * 8);
                    int by_n = (j->h * c->vs + j->max_vs * 8 - 1) / (j->max_vs * 8);
                    if (bx_n > c->bw) bx_n = c->bw;
                    if (by_n > c->bh) by_n = c->bh;
                    for (int by = 0; by < by_n && !err; by++) {
                        for (int bx = 0; bx < bx_n && !err; bx++) {
                            if (bjpg_decode_block_baseline(j, c, blk) != 0) { err = -10; break; }
                            bjpg_idct_8x8(blk, j->qt[c->tq], c->plane + ((size_t)by * 8u) * ((size_t)c->bw * 8u) + (size_t)bx * 8u, c->bw * 8);
                            bjpg_handle_restart(j, &todo);
                        }
                    }
                } else {
                    for (int my = 0; my < mcus_y && !err; my++) {
                        for (int mx = 0; mx < mcus_x && !err; mx++) {
                            for (int si = 0; si < ns && !err; si++) {
                                bjpg_comp_t *c = &j->comp[sc_idx[si]];
                                for (int vy = 0; vy < c->vs && !err; vy++) {
                                    for (int hx = 0; hx < c->hs && !err; hx++) {
                                        if (bjpg_decode_block_baseline(j, c, blk) != 0) { err = -10; break; }
                                        int bx = mx * c->hs + hx;
                                        int by = my * c->vs + vy;
                                        bjpg_idct_8x8(blk, j->qt[c->tq],
                                            c->plane + ((size_t)by * 8u) * ((size_t)c->bw * 8u) + (size_t)bx * 8u,
                                            c->bw * 8);
                                    }
                                }
                            }
                            bjpg_handle_restart(j, &todo);
                        }
                    }
                }
            } else {
                /* Progressive scan */
                if (ns == 1) {
                    bjpg_comp_t *c = &j->comp[sc_idx[0]];
                    int bx_n = (j->w * c->hs + j->max_hs * 8 - 1) / (j->max_hs * 8);
                    int by_n = (j->h * c->vs + j->max_vs * 8 - 1) / (j->max_vs * 8);
                    if (bx_n > c->bw) bx_n = c->bw;
                    if (by_n > c->bh) by_n = c->bh;
                    for (int by = 0; by < by_n && !err; by++) {
                        for (int bx = 0; bx < bx_n && !err; bx++) {
                            int16_t *blk = c->coeffs + (((size_t)by * (size_t)c->bw + (size_t)bx) * 64u);
                            if (bjpg_decode_block_prog(j, c, blk, ss, se, ah, al) != 0) { err = -11; break; }
                            bjpg_handle_restart(j, &todo);
                        }
                    }
                } else {
                    int mcu_w = j->max_hs * 8, mcu_h = j->max_vs * 8;
                    int mcus_x = (j->w + mcu_w - 1) / mcu_w;
                    int mcus_y = (j->h + mcu_h - 1) / mcu_h;
                    for (int my = 0; my < mcus_y && !err; my++) {
                        for (int mx = 0; mx < mcus_x && !err; mx++) {
                            for (int si = 0; si < ns && !err; si++) {
                                bjpg_comp_t *c = &j->comp[sc_idx[si]];
                                for (int vy = 0; vy < c->vs && !err; vy++) {
                                    for (int hx = 0; hx < c->hs && !err; hx++) {
                                        int bx = mx * c->hs + hx;
                                        int by = my * c->vs + vy;
                                        int16_t *blk = c->coeffs + (((size_t)by * (size_t)c->bw + (size_t)bx) * 64u);
                                        if (bjpg_decode_block_prog(j, c, blk, ss, se, ah, al) != 0) { err = -11; break; }
                                    }
                                }
                            }
                            bjpg_handle_restart(j, &todo);
                        }
                    }
                }
            }
            got_sos = 1;
        }
    }

    uint32_t *px = 0;
    if (!err && got_sof && got_sos) {
        if (j->progressive) {
            for (int ci = 0; ci < j->ncomp; ci++) {
                bjpg_comp_t *c = &j->comp[ci];
                for (int by = 0; by < c->bh; by++) {
                    for (int bx = 0; bx < c->bw; bx++) {
                        const int16_t *blk = c->coeffs + (((size_t)by * (size_t)c->bw + (size_t)bx) * 64u);
                        bjpg_idct_8x8(blk, j->qt[c->tq],
                            c->plane + ((size_t)by * 8u) * ((size_t)c->bw * 8u) + (size_t)bx * 8u,
                            c->bw * 8);
                    }
                }
            }
        }
        px = (uint32_t *)alloc_fn((size_t)j->w * (size_t)j->h * 4u);
        if (!px) err = -12;
        else if (j->ncomp == 1) {
            bjpg_comp_t *c0 = &j->comp[0];
            int st0 = c0->bw * 8;
            for (int y = 0; y < j->h; y++) {
                const uint8_t *row = c0->plane + (size_t)y * (size_t)st0;
                uint32_t *dst = px + (size_t)y * (size_t)j->w;
                for (int x = 0; x < j->w; x++) {
                    uint8_t v = row[x];
                    dst[x] = 0xFF000000u | ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;
                }
            }
        } else {
            bjpg_comp_t *cy = &j->comp[0], *ccb = &j->comp[1], *ccr = &j->comp[2];
            int sy_st = cy->bw * 8, scb_st = ccb->bw * 8, scr_st = ccr->bw * 8;
            int y_sx = j->max_hs / cy->hs,  y_sy = j->max_vs / cy->vs;
            int cb_sx = j->max_hs / ccb->hs, cb_sy = j->max_vs / ccb->vs;
            int cr_sx = j->max_hs / ccr->hs, cr_sy = j->max_vs / ccr->vs;
            for (int y = 0; y < j->h; y++) {
                const uint8_t *ry  = cy->plane  + (size_t)(y / y_sy)  * (size_t)sy_st;
                const uint8_t *rcb = ccb->plane + (size_t)(y / cb_sy) * (size_t)scb_st;
                const uint8_t *rcr = ccr->plane + (size_t)(y / cr_sy) * (size_t)scr_st;
                uint32_t *dst = px + (size_t)y * (size_t)j->w;
                for (int x = 0; x < j->w; x++) {
                    int Y  = ry[x / y_sx];
                    int Cb = rcb[x / cb_sx] - 128;
                    int Cr = rcr[x / cr_sx] - 128;
                    int R = Y + ((91881 * Cr + 32768) >> 16);
                    int G = Y - ((22554 * Cb + 46802 * Cr + 32768) >> 16);
                    int B = Y + ((116130 * Cb + 32768) >> 16);
                    dst[x] = 0xFF000000u |
                             ((uint32_t)bimg_clamp8(R) << 16) |
                             ((uint32_t)bimg_clamp8(G) << 8)  |
                             (uint32_t)bimg_clamp8(B);
                }
            }
        }
    } else if (!err) {
        err = -13;
    }

    int rw = j->w, rh = j->h;
    for (int i = 0; i < 4; i++) {
        if (j->comp[i].plane)  free_fn(j->comp[i].plane);
        if (j->comp[i].coeffs) free_fn(j->comp[i].coeffs);
    }
    free_fn(j);
    if (err || !px) { if (px) free_fn(px); return err ? err : -14; }
    *out_px = px; *out_w = rw; *out_h = rh;
    return 0;
}

/* ==========================================================================
 * 4. Unified Raster Image Dispatcher (PNG + JPEG + GIF + BMP)
 * ========================================================================== */
static int browser_img_decode(const uint8_t *data, uint32_t len,
                              void *(*alloc_fn)(size_t), void (*free_fn)(void *),
                              uint32_t **out_px, int *out_w, int *out_h) {
    if (!data || len < 4 || !alloc_fn || !free_fn || !out_px || !out_w || !out_h) return -1;
    /* PNG */
    if (len >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G')
        return png_decode_argb(data, len, alloc_fn, free_fn, out_px, out_w, out_h);
    /* JPEG (Baseline & Progressive) */
    if (data[0] == 0xFF && data[1] == 0xD8)
        return bimg_decode_jpeg(data, len, alloc_fn, free_fn, out_px, out_w, out_h);
    /* GIF87a / GIF89a */
    if (len >= 14 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F' && data[3] == '8')
        return bimg_decode_gif(data, len, alloc_fn, free_fn, out_px, out_w, out_h);
    /* BMP */
    if (len >= 54 && data[0] == 'B' && data[1] == 'M')
        return bimg_decode_bmp(data, len, alloc_fn, free_fn, out_px, out_w, out_h);
    return -1;
}

#endif /* ARCTIAN_BROWSER_IMG_H */
