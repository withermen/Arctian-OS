#ifndef ARCTIAN_MINIZ_H
#define ARCTIAN_MINIZ_H

/*
 * Arctian Freestanding PKZIP + RFC 1951 Deflate/Inflate + IEEE CRC32
 * Zero libc/stdio dependencies; operates strictly on asi_t (fs + mem).
 */

#include <stdint.h>
#include <stddef.h>
#include "asi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MZ_MAX_PATH 256
#define MZ_MAX_ENTRIES 1024

/* ---------------- Small freestanding helpers ---------------- */
static inline void mz_memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

static inline void mz_memset(void *dst, int c, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = (uint8_t)c;
}

static inline uint32_t mz_strlen(const char *s) {
    uint32_t n = 0;
    if (s) while (s[n]) n++;
    return n;
}

static inline void mz_strcpy(char *dst, uint32_t cap, const char *src) {
    uint32_t i = 0;
    if (!cap) return;
    if (!src) { dst[0] = 0; return; }
    for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

static inline void mz_strcat(char *dst, uint32_t cap, const char *src) {
    uint32_t i = mz_strlen(dst), j = 0;
    if (!src || !cap) return;
    for (; src[j] && i < cap - 1; i++, j++) dst[i] = src[j];
    dst[i] = 0;
}

static inline uint16_t mz_rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t mz_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void mz_wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline void mz_wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* ---------------- IEEE CRC32 ---------------- */
static inline uint32_t mz_crc32(uint32_t crc, const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = crc ^ 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        c ^= (uint32_t)p[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = (uint32_t)(-(int32_t)(c & 1u));
            c = (c >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return c ^ 0xFFFFFFFFu;
}

/* ---------------- RFC 1951 Inflate ---------------- */
#define MZ_MAXBITS 15

typedef struct {
    const uint8_t *in;
    uint32_t in_len;
    uint32_t in_pos;
    uint32_t bitbuf;
    int      bitcnt;
} mz_ibits_t;

typedef struct {
    int16_t count[MZ_MAXBITS + 1];
    int16_t symbol[288];
} mz_huff_t;

static inline int mz_get_bits(mz_ibits_t *s, int need) {
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->in_pos >= s->in_len) return -1;
        val |= (uint32_t)s->in[s->in_pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1u));
}

static inline int mz_huff_build(mz_huff_t *h, const int16_t *lens, int n) {
    int16_t offs[MZ_MAXBITS + 1];
    for (int i = 0; i <= MZ_MAXBITS; i++) h->count[i] = 0;
    for (int sym = 0; sym < n; sym++) h->count[lens[sym]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= MZ_MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;
    }
    offs[1] = 0;
    for (int len = 1; len < MZ_MAXBITS; len++)
        offs[len + 1] = (int16_t)(offs[len] + h->count[len]);
    for (int sym = 0; sym < n; sym++)
        if (lens[sym] != 0) h->symbol[offs[lens[sym]]++] = (int16_t)sym;
    return 0;
}

static inline int mz_huff_decode(mz_ibits_t *s, const mz_huff_t *h) {
    int len = 1, code = 0, first = 0, index = 0;
    for (;;) {
        int b = mz_get_bits(s, 1);
        if (b < 0) return -1;
        code |= b;
        int count = h->count[len];
        if (code - first < count) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        len++;
        if (len > MZ_MAXBITS) return -1;
    }
}

static inline int mz_inflate_codes(mz_ibits_t *s, uint8_t *out, uint32_t out_cap,
                                   uint32_t *out_pos,
                                   const mz_huff_t *lenc, const mz_huff_t *distc) {
    static const int16_t len_base[29] = {
        3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
    };
    static const int16_t len_ext[29] = {
        0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
    };
    static const int16_t dist_base[30] = {
        1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,
        3073,4097,6145,8193,12289,16385,24577
    };
    static const int16_t dist_ext[30] = {
        0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
    };
    for (;;) {
        int sym = mz_huff_decode(s, lenc);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (*out_pos >= out_cap) return -1;
            out[(*out_pos)++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int e = len_ext[sym];
            int len = len_base[sym];
            if (e) {
                int b = mz_get_bits(s, e);
                if (b < 0) return -1;
                len += b;
            }
            int dsym = mz_huff_decode(s, distc);
            if (dsym < 0 || dsym >= 30) return -1;
            int de = dist_ext[dsym];
            int dist = dist_base[dsym];
            if (de) {
                int b = mz_get_bits(s, de);
                if (b < 0) return -1;
                dist += b;
            }
            if (dist <= 0 || (uint32_t)dist > *out_pos) return -1;
            for (int k = 0; k < len; k++) {
                if (*out_pos >= out_cap) return -1;
                out[*out_pos] = out[*out_pos - (uint32_t)dist];
                (*out_pos)++;
            }
        }
    }
}

static inline int mz_inflate_raw(const uint8_t *in, uint32_t in_len,
                                 uint8_t *out, uint32_t out_cap, uint32_t *out_len) {
    static const int16_t clc_order[19] = {
        16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
    };
    mz_ibits_t s;
    s.in = in; s.in_len = in_len; s.in_pos = 0; s.bitbuf = 0; s.bitcnt = 0;
    uint32_t pos = 0;
    int last = 0;

    do {
        last = mz_get_bits(&s, 1);
        if (last < 0) return -1;
        int type = mz_get_bits(&s, 2);
        if (type < 0) return -1;

        if (type == 0) {
            s.bitbuf = 0; s.bitcnt = 0;
            if (s.in_pos + 4 > s.in_len) return -1;
            uint32_t len = (uint32_t)s.in[s.in_pos] | ((uint32_t)s.in[s.in_pos + 1] << 8);
            uint32_t nlen = (uint32_t)s.in[s.in_pos + 2] | ((uint32_t)s.in[s.in_pos + 3] << 8);
            s.in_pos += 4;
            if ((len ^ 0xFFFFu) != nlen) return -1;
            if (s.in_pos + len > s.in_len || pos + len > out_cap) return -1;
            mz_memcpy(out + pos, s.in + s.in_pos, len);
            s.in_pos += len;
            pos += len;
        } else if (type == 1) {
            mz_huff_t lenc, distc;
            int16_t lens[288];
            int i;
            for (i = 0; i < 144; i++) lens[i] = 8;
            for (; i < 256; i++) lens[i] = 9;
            for (; i < 280; i++) lens[i] = 7;
            for (; i < 288; i++) lens[i] = 8;
            if (mz_huff_build(&lenc, lens, 288) != 0) return -1;
            for (i = 0; i < 30; i++) lens[i] = 5;
            if (mz_huff_build(&distc, lens, 30) != 0) return -1;
            if (mz_inflate_codes(&s, out, out_cap, &pos, &lenc, &distc) != 0) return -1;
        } else if (type == 2) {
            mz_huff_t lenc, distc;
            int16_t lens[288 + 32];
            int hlit = mz_get_bits(&s, 5); if (hlit < 0) return -1; hlit += 257;
            int hdist = mz_get_bits(&s, 5); if (hdist < 0) return -1; hdist += 1;
            int hclen = mz_get_bits(&s, 4); if (hclen < 0) return -1; hclen += 4;
            if (hlit > 286 || hdist > 30) return -1;

            for (int i = 0; i < 288 + 32; i++) lens[i] = 0;
            for (int i = 0; i < hclen; i++) {
                int b = mz_get_bits(&s, 3);
                if (b < 0) return -1;
                lens[clc_order[i]] = (int16_t)b;
            }
            if (mz_huff_build(&lenc, lens, 19) != 0) return -1;

            int idx = 0;
            while (idx < hlit + hdist) {
                int sym = mz_huff_decode(&s, &lenc);
                if (sym < 0) return -1;
                if (sym < 16) {
                    lens[idx++] = (int16_t)sym;
                } else if (sym == 16) {
                    if (idx == 0) return -1;
                    int prev = lens[idx - 1];
                    int b = mz_get_bits(&s, 2);
                    if (b < 0) return -1;
                    int rep = b + 3;
                    while (rep-- && idx < hlit + hdist) lens[idx++] = (int16_t)prev;
                } else if (sym == 17) {
                    int b = mz_get_bits(&s, 3);
                    if (b < 0) return -1;
                    int rep = b + 3;
                    while (rep-- && idx < hlit + hdist) lens[idx++] = 0;
                } else {
                    int b = mz_get_bits(&s, 7);
                    if (b < 0) return -1;
                    int rep = b + 11;
                    while (rep-- && idx < hlit + hdist) lens[idx++] = 0;
                }
            }
            if (mz_huff_build(&lenc, lens, hlit) != 0) return -1;
            if (mz_huff_build(&distc, lens + hlit, hdist) != 0) return -1;
            if (mz_inflate_codes(&s, out, out_cap, &pos, &lenc, &distc) != 0) return -1;
        } else {
            return -1;
        }
    } while (!last);

    if (out_len) *out_len = pos;
    return 0;
}

/* ---------------- RFC 1951 Deflate (LZ77 + Fixed Huffman) ---------------- */
typedef struct {
    uint8_t *out;
    uint32_t cap;
    uint32_t pos;
    uint32_t bitbuf;
    int      bitcnt;
    int      overflow;
} mz_obits_t;

static inline void mz_put_bits(mz_obits_t *o, uint32_t bits, int n) {
    if (o->overflow) return;
    o->bitbuf |= (bits << o->bitcnt);
    o->bitcnt += n;
    while (o->bitcnt >= 8) {
        if (o->pos >= o->cap) { o->overflow = 1; return; }
        o->out[o->pos++] = (uint8_t)(o->bitbuf & 0xFFu);
        o->bitbuf >>= 8;
        o->bitcnt -= 8;
    }
}

static inline void mz_put_bits_msb(mz_obits_t *o, uint32_t code, int n) {
    uint32_t rev = 0;
    for (int i = 0; i < n; i++) {
        rev = (rev << 1) | (code & 1u);
        code >>= 1;
    }
    mz_put_bits(o, rev, n);
}

static inline void mz_flush_bits(mz_obits_t *o) {
    if (o->overflow) return;
    if (o->bitcnt > 0) {
        if (o->pos >= o->cap) { o->overflow = 1; return; }
        o->out[o->pos++] = (uint8_t)(o->bitbuf & 0xFFu);
        o->bitbuf = 0;
        o->bitcnt = 0;
    }
}

static inline void mz_emit_fixed_lit(mz_obits_t *o, int sym) {
    if (sym <= 143) {
        mz_put_bits_msb(o, (uint32_t)(0x30 + sym), 8);
    } else if (sym <= 255) {
        mz_put_bits_msb(o, (uint32_t)(0x190 + (sym - 144)), 9);
    } else if (sym <= 279) {
        mz_put_bits_msb(o, (uint32_t)(sym - 256), 7);
    } else {
        mz_put_bits_msb(o, (uint32_t)(0xC0 + (sym - 280)), 8);
    }
}

static inline void mz_emit_match_len(mz_obits_t *o, uint32_t len) {
    static const uint16_t lbase[29] = {
        3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
    };
    static const uint8_t lext[29] = {
        0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
    };
    int idx = 28;
    for (int i = 0; i < 28; i++) {
        if (len < lbase[i + 1]) { idx = i; break; }
    }
    mz_emit_fixed_lit(o, 257 + idx);
    if (lext[idx]) mz_put_bits(o, len - lbase[idx], lext[idx]);
}

static inline void mz_emit_match_dist(mz_obits_t *o, uint32_t dist) {
    static const uint16_t dbase[30] = {
        1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,
        3073,4097,6145,8193,12289,16385,24577
    };
    static const uint8_t dext[30] = {
        0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
    };
    int idx = 29;
    for (int i = 0; i < 29; i++) {
        if (dist < dbase[i + 1]) { idx = i; break; }
    }
    mz_put_bits_msb(o, (uint32_t)idx, 5);
    if (dext[idx]) mz_put_bits(o, dist - dbase[idx], dext[idx]);
}

static inline int mz_deflate_raw(const uint8_t *in, uint32_t in_len,
                                 uint8_t *out, uint32_t out_cap,
                                 uint32_t *out_len, asi_mem_t *mem) {
    mz_obits_t o;
    o.out = out; o.cap = out_cap; o.pos = 0; o.bitbuf = 0; o.bitcnt = 0; o.overflow = 0;

    if (in_len == 0) {
        mz_put_bits(&o, 1u, 1); /* BFINAL=1 */
        mz_put_bits(&o, 1u, 2); /* BTYPE=01 (fixed) */
        mz_emit_fixed_lit(&o, 256);
        mz_flush_bits(&o);
        if (o.overflow) return -1;
        if (out_len) *out_len = o.pos;
        return 0;
    }

    const uint32_t hash_sz = 4096u;
    int32_t *head = 0;
    if (mem && mem->alloc) {
        head = (int32_t *)mem->alloc(hash_sz * sizeof(int32_t));
        if (head) for (uint32_t i = 0; i < hash_sz; i++) head[i] = -1;
    }

    mz_put_bits(&o, 1u, 1); /* BFINAL=1 */
    mz_put_bits(&o, 1u, 2); /* BTYPE=01 (fixed Huffman) */

    uint32_t i = 0;
    while (i < in_len && !o.overflow) {
        uint32_t best_len = 0, best_dist = 0;
        if (head && i + 3 <= in_len) {
            uint32_t h = (((uint32_t)in[i] << 10) ^ ((uint32_t)in[i + 1] << 5) ^ (uint32_t)in[i + 2]) & (hash_sz - 1u);
            int32_t prev = head[h];
            head[h] = (int32_t)i;
            if (prev >= 0 && (i - (uint32_t)prev) <= 32768u && (i - (uint32_t)prev) >= 1u) {
                uint32_t p = (uint32_t)prev;
                uint32_t max_l = in_len - i;
                if (max_l > 258u) max_l = 258u;
                uint32_t l = 0;
                while (l < max_l && in[p + l] == in[i + l]) l++;
                if (l >= 3u) {
                    best_len = l;
                    best_dist = i - p;
                }
            }
        }
        if (best_len >= 3u) {
            mz_emit_match_len(&o, best_len);
            mz_emit_match_dist(&o, best_dist);
            if (head) {
                for (uint32_t k = 1; k < best_len && i + k + 3 <= in_len; k++) {
                    uint32_t h2 = (((uint32_t)in[i + k] << 10) ^ ((uint32_t)in[i + k + 1] << 5) ^ (uint32_t)in[i + k + 2]) & (hash_sz - 1u);
                    head[h2] = (int32_t)(i + k);
                }
            }
            i += best_len;
        } else {
            mz_emit_fixed_lit(&o, in[i]);
            i++;
        }
    }
    mz_emit_fixed_lit(&o, 256);
    mz_flush_bits(&o);
    if (head && mem && mem->free) mem->free(head);
    if (o.overflow) return -1;
    if (out_len) *out_len = o.pos;
    return 0;
}

/* ---------------- ASI File Helpers ---------------- */
static inline void mz_split_parent(const char *full, char *dir, uint32_t dcap,
                                   char *name, uint32_t ncap) {
    int last = -1;
    for (int i = 0; full[i]; i++) if (full[i] == '/' || full[i] == '\\') last = i;
    mz_strcpy(name, ncap, full + last + 1);
    if (last < 0) { mz_strcpy(dir, dcap, ""); return; }
    uint32_t n = (uint32_t)last;
    if (n >= dcap) n = dcap - 1;
    for (uint32_t i = 0; i < n; i++) dir[i] = full[i];
    dir[n] = 0;
    if (n == 2 && dir[1] == ':') mz_strcat(dir, dcap, "/");
}

static inline int mz_write_file(asi_t *asi, const char *path,
                                const uint8_t *data, uint32_t len) {
    if (!asi || !asi->fs || !asi->fs->create_in || !asi->fs->write) return -1;
    char dir[MZ_MAX_PATH], nm[96];
    mz_split_parent(path, dir, sizeof(dir), nm, sizeof(nm));
    if (!nm[0]) return -1;
    if (dir[0] && asi->fs->mkdir) asi->fs->mkdir(dir);
    void *h = dir[0] ? asi->fs->create_in(dir, nm, 0)
                     : (asi->fs->create ? asi->fs->create(0, nm, 0) : 0);
    if (!h) return -1;
    if (len == 0) return asi->fs->write(h, "", 0);
    return asi->fs->write(h, data, len);
}

/* ---------------- PKZIP Archive Creation ---------------- */
typedef struct {
    char     arc_name[MZ_MAX_PATH];
    uint32_t crc;
    uint32_t comp_size;
    uint32_t uncomp_size;
    uint32_t local_off;
    uint16_t method;
    uint8_t  is_dir;
} mz_cd_rec_t;

typedef struct {
    asi_t       *asi;
    uint8_t     *buf;
    uint32_t     len;
    uint32_t     cap;
    mz_cd_rec_t *recs;
    int          rec_n;
    int          rec_cap;
    int          use_deflate;
} mz_writer_t;

static inline int mz_w_reserve(mz_writer_t *w, uint32_t extra) {
    if (w->len + extra <= w->cap) return 0;
    uint32_t ncap = w->cap ? w->cap * 2u : 16384u;
    while (ncap < w->len + extra) {
        if (ncap > 0x40000000u) return -1;
        ncap *= 2u;
    }
    uint8_t *nb = (uint8_t *)w->asi->mem->alloc(ncap);
    if (!nb) return -1;
    if (w->buf) {
        mz_memcpy(nb, w->buf, w->len);
        w->asi->mem->free(w->buf);
    }
    w->buf = nb;
    w->cap = ncap;
    return 0;
}

static inline int mz_w_add_entry(mz_writer_t *w, const char *arc_name,
                                 const uint8_t *data, uint32_t size, uint8_t is_dir) {
    if (w->rec_n >= w->rec_cap) return -1;
    uint32_t fn_len = mz_strlen(arc_name);
    if (!fn_len || fn_len > 250u) return -1;

    uint16_t method = 0;
    uint32_t crc = 0;
    uint32_t comp_size = size;
    uint8_t *comp_buf = 0;
    const uint8_t *payload = data;

    if (!is_dir && size > 0) {
        crc = mz_crc32(0, data, size);
        if (w->use_deflate && size >= 16u) {
            comp_buf = (uint8_t *)w->asi->mem->alloc(size + 64u);
            if (comp_buf) {
                uint32_t clen = 0;
                if (mz_deflate_raw(data, size, comp_buf, size + 64u, &clen, w->asi->mem) == 0 && clen < size) {
                    method = 8;
                    comp_size = clen;
                    payload = comp_buf;
                }
            }
        }
    }

    if (mz_w_reserve(w, 30u + fn_len + comp_size) != 0) {
        if (comp_buf) w->asi->mem->free(comp_buf);
        return -1;
    }

    mz_cd_rec_t *r = &w->recs[w->rec_n++];
    mz_strcpy(r->arc_name, sizeof(r->arc_name), arc_name);
    r->crc = crc;
    r->comp_size = comp_size;
    r->uncomp_size = is_dir ? 0u : size;
    r->local_off = w->len;
    r->method = method;
    r->is_dir = is_dir;

    uint8_t *lh = w->buf + w->len;
    mz_wr32(lh + 0,  0x04034B50u);
    mz_wr16(lh + 4,  20u);
    mz_wr16(lh + 6,  0x0800u); /* UTF-8 */
    mz_wr16(lh + 8,  method);
    mz_wr16(lh + 10, 0u);
    mz_wr16(lh + 12, 0x5421u);
    mz_wr32(lh + 14, crc);
    mz_wr32(lh + 18, comp_size);
    mz_wr32(lh + 22, r->uncomp_size);
    mz_wr16(lh + 26, (uint16_t)fn_len);
    mz_wr16(lh + 28, 0u);
    mz_memcpy(lh + 30, arc_name, fn_len);
    w->len += 30u + fn_len;
    if (!is_dir && comp_size > 0 && payload) {
        mz_memcpy(w->buf + w->len, payload, comp_size);
        w->len += comp_size;
    }
    if (comp_buf) w->asi->mem->free(comp_buf);
    return 0;
}

static inline int mz_w_walk(mz_writer_t *w, const char *fs_path,
                            const char *arc_prefix, int depth) {
    if (depth > 12) return -1;
    asi_fs_t *fs = w->asi->fs;
    asi_stat_t st;
    if (!fs->stat || fs->stat(fs_path, &st) != 0) return -1;

    if (!st.is_dir) {
        uint8_t *fbuf = 0;
        if (st.size > 0) {
            fbuf = (uint8_t *)w->asi->mem->alloc(st.size);
            if (!fbuf) return -1;
            uint32_t got = fs->read_path(fs_path, fbuf, st.size);
            if (got != st.size) { w->asi->mem->free(fbuf); return -1; }
        }
        int rc = mz_w_add_entry(w, arc_prefix, fbuf, st.size, 0);
        if (fbuf) w->asi->mem->free(fbuf);
        return rc;
    }

    char dname[MZ_MAX_PATH];
    mz_strcpy(dname, sizeof(dname), arc_prefix);
    uint32_t dl = mz_strlen(dname);
    if (dl > 0 && dname[dl - 1] != '/') mz_strcat(dname, sizeof(dname), "/");
    if (mz_w_add_entry(w, dname, 0, 0, 1) != 0) return -1;

    if (!fs->count_dir || !fs->list_dir) return 0;
    int cnt = fs->count_dir(fs_path);
    for (int i = 0; i < cnt; i++) {
        char nm[96];
        int isd = 0;
        if (fs->list_dir(fs_path, i, nm, sizeof(nm), &isd) != 0 || !nm[0]) continue;
        char cpath[MZ_MAX_PATH], carcn[MZ_MAX_PATH];
        mz_strcpy(cpath, sizeof(cpath), fs_path);
        uint32_t pl = mz_strlen(cpath);
        if (pl > 0 && cpath[pl - 1] != '/') mz_strcat(cpath, sizeof(cpath), "/");
        mz_strcat(cpath, sizeof(cpath), nm);

        mz_strcpy(carcn, sizeof(carcn), dname);
        mz_strcat(carcn, sizeof(carcn), nm);
        if (mz_w_walk(w, cpath, carcn, depth + 1) != 0) return -1;
    }
    return 0;
}

static inline int mz_zip_create(asi_t *asi, const char *out_zip,
                                const char * const *src_paths, int n_paths,
                                int use_deflate, int *out_count) {
    if (!asi || !asi->fs || !asi->mem || !out_zip || !src_paths || n_paths <= 0) return -1;
    mz_writer_t w;
    mz_memset(&w, 0, sizeof(w));
    w.asi = asi;
    w.use_deflate = use_deflate ? 1 : 0;
    w.rec_cap = MZ_MAX_ENTRIES;
    w.recs = (mz_cd_rec_t *)asi->mem->alloc((uint32_t)w.rec_cap * sizeof(mz_cd_rec_t));
    if (!w.recs) return -1;

    int err = 0;
    for (int i = 0; i < n_paths; i++) {
        const char *sp = src_paths[i];
        if (!sp || !sp[0]) continue;
        char clean[MZ_MAX_PATH];
        mz_strcpy(clean, sizeof(clean), sp);
        uint32_t cl = mz_strlen(clean);
        while (cl > 3 && clean[cl - 1] == '/') clean[--cl] = 0;
        const char *base = clean;
        for (const char *p = clean; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
        if (!base[0]) { err = -1; break; }
        if (mz_w_walk(&w, clean, base, 0) != 0) { err = -1; break; }
    }

    if (!err && w.rec_n > 0) {
        uint32_t cd_off = w.len;
        for (int i = 0; i < w.rec_n; i++) {
            mz_cd_rec_t *r = &w.recs[i];
            uint32_t fn_len = mz_strlen(r->arc_name);
            if (mz_w_reserve(&w, 46u + fn_len) != 0) { err = -1; break; }
            uint8_t *cd = w.buf + w.len;
            mz_wr32(cd + 0,  0x02014B50u);
            mz_wr16(cd + 4,  20u);
            mz_wr16(cd + 6,  20u);
            mz_wr16(cd + 8,  0x0800u);
            mz_wr16(cd + 10, r->method);
            mz_wr16(cd + 12, 0u);
            mz_wr16(cd + 14, 0x5421u);
            mz_wr32(cd + 16, r->crc);
            mz_wr32(cd + 20, r->comp_size);
            mz_wr32(cd + 24, r->uncomp_size);
            mz_wr16(cd + 28, (uint16_t)fn_len);
            mz_wr16(cd + 30, 0u);
            mz_wr16(cd + 32, 0u);
            mz_wr16(cd + 34, 0u);
            mz_wr16(cd + 36, 0u);
            mz_wr32(cd + 38, r->is_dir ? 0x10u : 0x20u);
            mz_wr32(cd + 42, r->local_off);
            mz_memcpy(cd + 46, r->arc_name, fn_len);
            w.len += 46u + fn_len;
        }
        if (!err) {
            uint32_t cd_size = w.len - cd_off;
            if (mz_w_reserve(&w, 22u) != 0) err = -1;
            else {
                uint8_t *eocd = w.buf + w.len;
                mz_wr32(eocd + 0,  0x06054B50u);
                mz_wr16(eocd + 4,  0u);
                mz_wr16(eocd + 6,  0u);
                mz_wr16(eocd + 8,  (uint16_t)w.rec_n);
                mz_wr16(eocd + 10, (uint16_t)w.rec_n);
                mz_wr32(eocd + 12, cd_size);
                mz_wr32(eocd + 16, cd_off);
                mz_wr16(eocd + 20, 0u);
                w.len += 22u;
                if (mz_write_file(asi, out_zip, w.buf, w.len) != 0) err = -1;
            }
        }
    } else if (w.rec_n == 0) {
        err = -1;
    }

    if (out_count) *out_count = err == 0 ? w.rec_n : 0;
    if (w.buf) asi->mem->free(w.buf);
    if (w.recs) asi->mem->free(w.recs);
    return err;
}

/* ---------------- PKZIP Archive Inspection & Extraction ---------------- */
static inline int mz_sanitize_rel(const char *raw, uint32_t rlen,
                                  char *out, uint32_t cap) {
    uint32_t o = 0;
    uint32_t i = 0;
    while (i < rlen && (raw[i] == '/' || raw[i] == '\\')) i++;
    while (i < rlen && o < cap - 1) {
        char seg[96];
        uint32_t sl = 0;
        while (i < rlen && raw[i] != '/' && raw[i] != '\\') {
            if (sl < sizeof(seg) - 1) seg[sl++] = raw[i];
            i++;
        }
        seg[sl] = 0;
        while (i < rlen && (raw[i] == '/' || raw[i] == '\\')) i++;
        if (sl == 0 || (sl == 1 && seg[0] == '.')) continue;
        if (sl == 2 && seg[0] == '.' && seg[1] == '.') return -1;
        if (o > 0 && o < cap - 1) out[o++] = '/';
        for (uint32_t k = 0; k < sl && o < cap - 1; k++) out[o++] = seg[k];
    }
    out[o] = 0;
    return o > 0 ? 0 : -1;
}

static inline int mz_find_eocd(const uint8_t *buf, uint32_t len, uint32_t *eocd_off) {
    if (len < 22u) return -1;
    uint32_t min_off = (len > 65557u) ? (len - 65557u) : 0u;
    for (int64_t pos = (int64_t)len - 22; pos >= (int64_t)min_off; pos--) {
        if (mz_rd32(buf + pos) == 0x06054B50u) {
            uint16_t clen = mz_rd16(buf + pos + 20);
            if ((uint32_t)pos + 22u + clen == len) {
                *eocd_off = (uint32_t)pos;
                return 0;
            }
        }
    }
    return -1;
}

typedef struct {
    char     name[MZ_MAX_PATH];
    uint32_t crc;
    uint32_t comp_size;
    uint32_t uncomp_size;
    uint16_t method;
    uint8_t  is_dir;
} mz_zip_info_t;

static inline int mz_zip_list(asi_t *asi, const char *zip_path,
                              mz_zip_info_t *out, int max_out, int *total_out) {
    if (!asi || !asi->fs || !asi->mem || !zip_path) return -1;
    asi_stat_t st;
    if (!asi->fs->stat || asi->fs->stat(zip_path, &st) != 0 || st.is_dir || st.size < 22u)
        return -1;
    uint8_t *zbuf = (uint8_t *)asi->mem->alloc(st.size);
    if (!zbuf) return -1;
    if (asi->fs->read_path(zip_path, zbuf, st.size) != st.size) {
        asi->mem->free(zbuf);
        return -1;
    }
    uint32_t eoff = 0;
    if (mz_find_eocd(zbuf, st.size, &eoff) != 0) {
        asi->mem->free(zbuf);
        return -1;
    }
    uint16_t entries = mz_rd16(zbuf + eoff + 10);
    uint32_t cd_off = mz_rd32(zbuf + eoff + 16);
    uint32_t pos = cd_off;
    int shown = 0;
    for (uint16_t i = 0; i < entries && pos + 46u <= st.size; i++) {
        if (mz_rd32(zbuf + pos) != 0x02014B50u) break;
        uint16_t fn_len = mz_rd16(zbuf + pos + 28);
        uint16_t ex_len = mz_rd16(zbuf + pos + 30);
        uint16_t cm_len = mz_rd16(zbuf + pos + 32);
        if (pos + 46u + fn_len + ex_len + cm_len > st.size) break;
        if (out && shown < max_out) {
            mz_zip_info_t *inf = &out[shown];
            uint32_t cp = fn_len < MZ_MAX_PATH - 1 ? fn_len : MZ_MAX_PATH - 1;
            mz_memcpy(inf->name, zbuf + pos + 46, cp);
            inf->name[cp] = 0;
            inf->method = mz_rd16(zbuf + pos + 10);
            inf->crc = mz_rd32(zbuf + pos + 16);
            inf->comp_size = mz_rd32(zbuf + pos + 20);
            inf->uncomp_size = mz_rd32(zbuf + pos + 24);
            inf->is_dir = (cp > 0 && inf->name[cp - 1] == '/') ? 1 : 0;
            shown++;
        }
        pos += 46u + fn_len + ex_len + cm_len;
    }
    if (total_out) *total_out = (int)entries;
    asi->mem->free(zbuf);
    return 0;
}

static inline int mz_zip_extract(asi_t *asi, const char *zip_path,
                                 const char *dest_dir, int *out_count) {
    if (!asi || !asi->fs || !asi->mem || !zip_path || !dest_dir) return -1;
    asi_stat_t st;
    if (!asi->fs->stat || asi->fs->stat(zip_path, &st) != 0 || st.is_dir || st.size < 22u)
        return -1;
    uint8_t *zbuf = (uint8_t *)asi->mem->alloc(st.size);
    if (!zbuf) return -1;
    if (asi->fs->read_path(zip_path, zbuf, st.size) != st.size) {
        asi->mem->free(zbuf);
        return -1;
    }

    uint32_t eoff = 0;
    if (mz_find_eocd(zbuf, st.size, &eoff) != 0) {
        asi->mem->free(zbuf);
        return -1;
    }

    uint16_t entries = mz_rd16(zbuf + eoff + 10);
    uint32_t cd_off = mz_rd32(zbuf + eoff + 16);
    if (cd_off >= st.size) { asi->mem->free(zbuf); return -1; }

    char droot[MZ_MAX_PATH];
    mz_strcpy(droot, sizeof(droot), dest_dir);
    uint32_t drl = mz_strlen(droot);
    while (drl > 3 && droot[drl - 1] == '/') droot[--drl] = 0;
    if (asi->fs->mkdir) asi->fs->mkdir(droot);

    uint32_t pos = cd_off;
    int ok_cnt = 0;
    int err = 0;

    for (uint16_t i = 0; i < entries; i++) {
        if (pos + 46u > st.size || mz_rd32(zbuf + pos) != 0x02014B50u) { err = -1; break; }
        uint16_t method = mz_rd16(zbuf + pos + 10);
        uint32_t crc = mz_rd32(zbuf + pos + 16);
        uint32_t comp_sz = mz_rd32(zbuf + pos + 20);
        uint32_t uncomp_sz = mz_rd32(zbuf + pos + 24);
        uint16_t fn_len = mz_rd16(zbuf + pos + 28);
        uint16_t ex_len = mz_rd16(zbuf + pos + 30);
        uint16_t cm_len = mz_rd16(zbuf + pos + 32);
        uint32_t lhoff = mz_rd32(zbuf + pos + 42);
        if (pos + 46u + fn_len + ex_len + cm_len > st.size) { err = -1; break; }

        const char *raw_name = (const char *)(zbuf + pos + 46);
        int is_dir = (fn_len > 0 && (raw_name[fn_len - 1] == '/' || raw_name[fn_len - 1] == '\\'));
        pos += 46u + fn_len + ex_len + cm_len;

        char rel[MZ_MAX_PATH];
        if (mz_sanitize_rel(raw_name, fn_len, rel, sizeof(rel)) != 0) continue;

        char full[MZ_MAX_PATH];
        mz_strcpy(full, sizeof(full), droot);
        uint32_t fl = mz_strlen(full);
        if (fl > 0 && full[fl - 1] != '/') mz_strcat(full, sizeof(full), "/");
        mz_strcat(full, sizeof(full), rel);

        if (is_dir) {
            if (asi->fs->mkdir) asi->fs->mkdir(full);
            ok_cnt++;
            continue;
        }

        if (lhoff + 30u > st.size || mz_rd32(zbuf + lhoff) != 0x04034B50u) { err = -1; break; }
        uint16_t lh_fn = mz_rd16(zbuf + lhoff + 26);
        uint16_t lh_ex = mz_rd16(zbuf + lhoff + 28);
        uint32_t data_off = lhoff + 30u + lh_fn + lh_ex;
        if (data_off + comp_sz > st.size) { err = -1; break; }

        const uint8_t *cdata = zbuf + data_off;
        if (method == 0) {
            if (comp_sz != uncomp_sz) { err = -1; break; }
            if (mz_crc32(0, cdata, uncomp_sz) != crc) { err = -1; break; }
            if (mz_write_file(asi, full, cdata, uncomp_sz) != 0) { err = -1; break; }
            ok_cnt++;
        } else if (method == 8) {
            uint8_t *ubuf = (uint8_t *)asi->mem->alloc(uncomp_sz ? uncomp_sz : 1u);
            if (!ubuf) { err = -1; break; }
            uint32_t got = 0;
            if (mz_inflate_raw(cdata, comp_sz, ubuf, uncomp_sz, &got) != 0 ||
                got != uncomp_sz ||
                mz_crc32(0, ubuf, uncomp_sz) != crc) {
                asi->mem->free(ubuf);
                err = -1;
                break;
            }
            int wrc = mz_write_file(asi, full, ubuf, uncomp_sz);
            asi->mem->free(ubuf);
            if (wrc != 0) { err = -1; break; }
            ok_cnt++;
        } else {
            err = -1;
            break;
        }
    }

    if (out_count) *out_count = ok_cnt;
    asi->mem->free(zbuf);
    return err;
}

#ifdef __cplusplus
}
#endif

#endif /* ARCTIAN_MINIZ_H */
