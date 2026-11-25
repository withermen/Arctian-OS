/*
 * Arctian Web Engine — Freestanding RFC 1951 / 1950 / 1952 Inflate (DEFLATE / ZLIB / GZIP)
 * Zero 3rd-party dependencies, zero libc, zero recursion.
 */
#ifndef ARCTIAN_INFLATE_GZIP_H
#define ARCTIAN_INFLATE_GZIP_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint16_t count[16];
    uint16_t symbol[288];
} inf_tree_t;

typedef struct {
    const uint8_t *src;
    uint32_t src_len;
    uint32_t pos;
    uint32_t bitbuf;
    int      bitcnt;

    uint8_t *dst;
    uint32_t dst_len;
    uint32_t dst_cap;
    uint32_t max_out;

    void *(*alloc_fn)(size_t);
    void  (*free_fn)(void *);
} inf_ctx_t;

static inline uint32_t inf_bits(inf_ctx_t *c, int n) {
    while (c->bitcnt < n) {
        if (c->pos >= c->src_len) { c->bitcnt += 8; continue; }
        c->bitbuf |= ((uint32_t)c->src[c->pos++]) << c->bitcnt;
        c->bitcnt += 8;
    }
    uint32_t v = c->bitbuf & ((1u << n) - 1u);
    c->bitbuf >>= n;
    c->bitcnt -= n;
    return v;
}

static int inf_build_tree(inf_tree_t *t, const uint8_t *lengths, int nsym) {
    uint16_t offs[16];
    for (int i = 0; i < 16; i++) t->count[i] = 0;
    for (int i = 0; i < nsym; i++) {
        if (lengths[i] > 15) return -1;
        t->count[lengths[i]]++;
    }
    t->count[0] = 0;
    uint16_t sum = 0;
    for (int i = 1; i < 16; i++) {
        offs[i] = sum;
        sum = (uint16_t)(sum + t->count[i]);
    }
    for (int i = 0; i < nsym; i++) {
        if (lengths[i]) t->symbol[offs[lengths[i]]++] = (uint16_t)i;
    }
    return 0;
}

static int inf_decode_sym(inf_ctx_t *c, const inf_tree_t *t) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= (int)inf_bits(c, 1);
        int cnt = t->count[len];
        if (code - cnt < first) return t->symbol[index + (code - first)];
        index += cnt;
        first = (first + cnt) << 1;
        code <<= 1;
    }
    return -1;
}

static int inf_ensure(inf_ctx_t *c, uint32_t need) {
    if (c->dst_len + need + 1u <= c->dst_cap) return 0;
    uint32_t ncap = c->dst_cap ? c->dst_cap * 2u : 16384u;
    while (ncap < c->dst_len + need + 1u) ncap *= 2u;
    if (ncap > c->max_out + 1u) ncap = c->max_out + 1u;
    if (c->dst_len + need + 1u > ncap) return -1;
    uint8_t *nb = (uint8_t *)c->alloc_fn(ncap);
    if (!nb) return -1;
    for (uint32_t i = 0; i < c->dst_len; i++) nb[i] = c->dst[i];
    if (c->dst) c->free_fn(c->dst);
    c->dst = nb;
    c->dst_cap = ncap;
    return 0;
}

static int inf_decode_block(inf_ctx_t *c, const inf_tree_t *lt, const inf_tree_t *dt) {
    static const uint16_t lbase[29] = {
        3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
        35,43,51,59,67,83,99,115,131,163,195,227,258
    };
    static const uint8_t lextra[29] = {
        0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
        3,3,3,3,4,4,4,4,5,5,5,5,0
    };
    static const uint16_t dbase[30] = {
        1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
        257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
    };
    static const uint8_t dextra[30] = {
        0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
        7,7,8,8,9,9,10,10,11,11,12,12,13,13
    };

    for (;;) {
        int sym = inf_decode_sym(c, lt);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (inf_ensure(c, 1) != 0) return -2;
            c->dst[c->dst_len++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            int li = sym - 257;
            if (li < 0 || li >= 29) return -3;
            uint32_t length = lbase[li] + inf_bits(c, lextra[li]);
            int dsym = inf_decode_sym(c, dt);
            if (dsym < 0 || dsym >= 30) return -4;
            uint32_t dist = dbase[dsym] + inf_bits(c, dextra[dsym]);
            if (dist == 0 || dist > c->dst_len) return -5;
            if (inf_ensure(c, length) != 0) return -2;
            uint32_t src_off = c->dst_len - dist;
            for (uint32_t k = 0; k < length; k++) {
                c->dst[c->dst_len++] = c->dst[src_off + k];
            }
        }
    }
}

static int inflate_raw(const uint8_t *src, uint32_t src_len,
                       void *(*alloc_fn)(size_t), void (*free_fn)(void *),
                       uint8_t **out_buf, uint32_t *out_len, uint32_t max_out) {
    inf_ctx_t c;
    c.src = src; c.src_len = src_len; c.pos = 0; c.bitbuf = 0; c.bitcnt = 0;
    c.dst = 0; c.dst_len = 0; c.dst_cap = 0; c.max_out = max_out;
    c.alloc_fn = alloc_fn; c.free_fn = free_fn;

    inf_tree_t *trees = (inf_tree_t *)alloc_fn(sizeof(inf_tree_t) * 2u);
    if (!trees) return -10;
    inf_tree_t *lt = &trees[0], *dt = &trees[1];

    int err = 0, bfinal = 0;
    while (!bfinal && !err) {
        bfinal = (int)inf_bits(&c, 1);
        int btype = (int)inf_bits(&c, 2);
        if (btype == 0) {
            /* Stored uncompressed block */
            c.bitbuf = 0; c.bitcnt = 0;
            if (c.pos + 4 > c.src_len) { err = -11; break; }
            uint16_t blen  = (uint16_t)(c.src[c.pos] | ((uint16_t)c.src[c.pos + 1] << 8));
            uint16_t nblen = (uint16_t)(c.src[c.pos + 2] | ((uint16_t)c.src[c.pos + 3] << 8));
            c.pos += 4;
            if ((uint16_t)(blen ^ 0xFFFFu) != nblen || c.pos + blen > c.src_len) { err = -12; break; }
            if (inf_ensure(&c, blen) != 0) { err = -13; break; }
            for (uint32_t k = 0; k < blen; k++) c.dst[c.dst_len++] = c.src[c.pos++];
        } else if (btype == 1) {
            /* Fixed Huffman */
            uint8_t lens[288];
            int i = 0;
            for (; i < 144; i++) lens[i] = 8;
            for (; i < 256; i++) lens[i] = 9;
            for (; i < 280; i++) lens[i] = 7;
            for (; i < 288; i++) lens[i] = 8;
            inf_build_tree(lt, lens, 288);
            for (i = 0; i < 32; i++) lens[i] = 5;
            inf_build_tree(dt, lens, 32);
            err = inf_decode_block(&c, lt, dt);
        } else if (btype == 2) {
            /* Dynamic Huffman */
            static const uint8_t cl_order[19] = {
                16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
            };
            int hlit  = (int)inf_bits(&c, 5) + 257;
            int hdist = (int)inf_bits(&c, 5) + 1;
            int hclen = (int)inf_bits(&c, 4) + 4;
            if (hlit > 286 || hdist > 30) { err = -14; break; }
            uint8_t cl_lens[19];
            for (int i = 0; i < 19; i++) cl_lens[i] = 0;
            for (int i = 0; i < hclen; i++) cl_lens[cl_order[i]] = (uint8_t)inf_bits(&c, 3);
            inf_build_tree(lt, cl_lens, 19);

            uint8_t lengths[320];
            int total = hlit + hdist, idx = 0;
            while (idx < total) {
                int sym = inf_decode_sym(&c, lt);
                if (sym < 0) { err = -15; break; }
                if (sym < 16) {
                    lengths[idx++] = (uint8_t)sym;
                } else {
                    uint8_t val = 0;
                    int rep = 0;
                    if (sym == 16) {
                        if (idx == 0) { err = -16; break; }
                        val = lengths[idx - 1];
                        rep = 3 + (int)inf_bits(&c, 2);
                    } else if (sym == 17) {
                        rep = 3 + (int)inf_bits(&c, 3);
                    } else if (sym == 18) {
                        rep = 11 + (int)inf_bits(&c, 7);
                    } else { err = -17; break; }
                    if (idx + rep > total) { err = -18; break; }
                    while (rep--) lengths[idx++] = val;
                }
            }
            if (!err) {
                inf_build_tree(lt, lengths, hlit);
                inf_build_tree(dt, lengths + hlit, hdist);
                err = inf_decode_block(&c, lt, dt);
            }
        } else {
            err = -19;
        }
    }

    free_fn(trees);
    if (err || !c.dst) {
        if (c.dst) free_fn(c.dst);
        return err ? err : -20;
    }
    c.dst[c.dst_len] = 0;
    *out_buf = c.dst;
    *out_len = c.dst_len;
    return 0;
}

static int inflate_gzip(const uint8_t *src, uint32_t src_len,
                        void *(*alloc_fn)(size_t), void (*free_fn)(void *),
                        uint8_t **out_buf, uint32_t *out_len, uint32_t max_out) {
    if (!src || src_len < 18 || src[0] != 0x1F || src[1] != 0x8B || src[2] != 8) return -1;
    uint8_t flg = src[3];
    uint32_t p = 10;
    if (flg & 0x04) { /* FEXTRA */
        if (p + 2 > src_len) return -2;
        uint16_t xlen = (uint16_t)(src[p] | ((uint16_t)src[p + 1] << 8));
        p += 2 + xlen;
    }
    if (flg & 0x08) { while (p < src_len && src[p]) p++; p++; } /* FNAME */
    if (flg & 0x10) { while (p < src_len && src[p]) p++; p++; } /* FCOMMENT */
    if (flg & 0x02) p += 2; /* FHCRC */
    if (p >= src_len) return -3;
    return inflate_raw(src + p, src_len - p, alloc_fn, free_fn, out_buf, out_len, max_out);
}

static int inflate_deflate_auto(const uint8_t *src, uint32_t src_len,
                                void *(*alloc_fn)(size_t), void (*free_fn)(void *),
                                uint8_t **out_buf, uint32_t *out_len, uint32_t max_out) {
    if (!src || src_len < 2) return -1;
    /* RFC 1950 zlib header check: CM=8, CINFO<=7, (CMF*256+FLG)%31==0 */
    if ((src[0] & 0x0F) == 8 && (src[0] >> 4) <= 7 &&
        ((((uint32_t)src[0] << 8) | src[1]) % 31u) == 0) {
        uint32_t p = (src[1] & 0x20) ? 6u : 2u;
        if (p < src_len) {
            if (inflate_raw(src + p, src_len - p, alloc_fn, free_fn, out_buf, out_len, max_out) == 0)
                return 0;
        }
    }
    return inflate_raw(src, src_len, alloc_fn, free_fn, out_buf, out_len, max_out);
}

#endif /* ARCTIAN_INFLATE_GZIP_H */
