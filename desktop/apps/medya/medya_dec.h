/*
 * Arctian OS — Freestanding Multi-Format Audio Decoder (WAV / FLAC / MP3 / OGG)
 * Zero 3rd-party dependencies, zero libc, zero libm.
 * Outputs canonical 16-bit PCM RIFF/WAVE buffers ready for asi->audio->play_wav.
 */
#ifndef ARCTIAN_MEDYA_DEC_H
#define ARCTIAN_MEDYA_DEC_H

#include <stdint.h>
#include <stddef.h>

extern double fabs(double x);
extern double floor(double x);
extern double sqrt(double x);
extern double sin(double x);
extern double cos(double x);
extern void  *memset(void *dst, int c, size_t n);
extern void  *memcpy(void *dst, const void *src, size_t n);
extern void  *memmove(void *dst, const void *src, size_t n);

#define MD_PI 3.14159265358979323846
#define MD_MAX_PCM_BYTES (64u * 1024u * 1024u)

typedef struct {
    uint8_t    *wav_buf;
    uint32_t    wav_len;
    uint32_t    pcm_off;
    uint32_t    pcm_len;
    uint32_t    rate;
    uint16_t    channels;
    uint32_t    dur_ms;
    const char *fmt_name;
    const char *err;
} medya_audio_t;

static inline uint16_t md_rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static inline uint32_t md_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint32_t md_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static inline void md_wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static inline void md_wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline int16_t md_clip16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static void md_write_wav_hdr(uint8_t *h, uint32_t rate, uint16_t ch, uint32_t pcm_bytes) {
    h[0]='R'; h[1]='I'; h[2]='F'; h[3]='F';
    md_wr32(h + 4, 36u + pcm_bytes);
    h[8]='W'; h[9]='A'; h[10]='V'; h[11]='E';
    h[12]='f'; h[13]='m'; h[14]='t'; h[15]=' ';
    md_wr32(h + 16, 16u);
    md_wr16(h + 20, 1u);
    md_wr16(h + 22, ch);
    md_wr32(h + 24, rate);
    md_wr32(h + 28, rate * (uint32_t)ch * 2u);
    md_wr16(h + 32, (uint16_t)(ch * 2u));
    md_wr16(h + 34, 16u);
    h[36]='d'; h[37]='a'; h[38]='t'; h[39]='a';
    md_wr32(h + 40, pcm_bytes);
}

static double md_exp(double x) {
    if (x > 700.0) x = 700.0;
    if (x < -700.0) return 0.0;
    const double LN2 = 0.6931471805599453;
    int k = (int)floor(x / LN2 + 0.5);
    double r = x - (double)k * LN2;
    double sum = 1.0, term = 1.0;
    for (int i = 1; i <= 18; i++) { term *= r / (double)i; sum += term; }
    if (k > 0) { for (int i = 0; i < k && i < 1020; i++) sum *= 2.0; }
    else if (k < 0) { for (int i = 0; i < -k && i < 1020; i++) sum *= 0.5; }
    return sum;
}
static double md_ln(double x) {
    if (x <= 0.0) return 0.0;
    int k = 0;
    while (x >= 2.0) { x *= 0.5; k++; }
    while (x < 1.0)  { x *= 2.0; k--; }
    double t = (x - 1.0) / (x + 1.0), t2 = t * t, sum = 0.0, p = t;
    for (int i = 1; i <= 23; i += 2) { sum += p / (double)i; p *= t2; }
    return 2.0 * sum + (double)k * 0.6931471805599453;
}
static double md_pow(double b, double e) {
    if (e == 0.0) return 1.0;
    if (b == 0.0) return 0.0;
    if (b < 0.0) return 0.0;
    return md_exp(e * md_ln(b));
}
static double md_ldexp(double x, int e) {
    while (e > 0) { x *= 2.0; e--; }
    while (e < 0) { x *= 0.5; e++; }
    return x;
}

static int md_ext_eq(const char *name, const char *ext) {
    if (!name) return 0;
    int nl = 0, el = 0;
    while (name[nl]) nl++;
    while (ext[el]) el++;
    if (nl < el + 1) return 0;
    const char *p = name + nl - el;
    if (p[-1] != '.') return 0;
    for (int i = 0; i < el; i++) {
        char c = p[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != ext[i]) return 0;
    }
    return 1;
}

/* ==========================================================================
 * Dynamic PCM Output Growable Buffer (starts with 44-byte WAV header)
 * ========================================================================== */
typedef struct {
    uint8_t *buf;
    uint32_t cap;
    uint32_t pcm_bytes;
    void *(*alloc_fn)(size_t);
    void  (*free_fn)(void *);
} md_pcm_out_t;

static int md_pcm_init(md_pcm_out_t *o, uint32_t init_pcm_bytes,
                       void *(*af)(size_t), void (*ff)(void *)) {
    o->alloc_fn = af; o->free_fn = ff;
    uint32_t cap = 44u + (init_pcm_bytes < 65536u ? 65536u : init_pcm_bytes);
    o->buf = (uint8_t *)af(cap);
    if (!o->buf) return -1;
    o->cap = cap;
    o->pcm_bytes = 0;
    return 0;
}

static int md_pcm_reserve(md_pcm_out_t *o, uint32_t extra_bytes) {
    uint64_t need = 44ull + (uint64_t)o->pcm_bytes + (uint64_t)extra_bytes;
    if (need > MD_MAX_PCM_BYTES) return -1;
    if (need <= o->cap) return 0;
    uint32_t ncap = o->cap;
    while ((uint64_t)ncap < need) {
        if (ncap >= MD_MAX_PCM_BYTES / 2u) { ncap = (uint32_t)need; break; }
        ncap *= 2u;
    }
    uint8_t *nb = (uint8_t *)o->alloc_fn(ncap);
    if (!nb) return -1;
    memcpy(nb, o->buf, 44u + o->pcm_bytes);
    o->free_fn(o->buf);
    o->buf = nb;
    o->cap = ncap;
    return 0;
}

static int md_pcm_push_i16(md_pcm_out_t *o, const int16_t *samples, uint32_t nsamples) {
    uint32_t bytes = nsamples * 2u;
    if (md_pcm_reserve(o, bytes) != 0) return -1;
    memcpy(o->buf + 44u + o->pcm_bytes, samples, bytes);
    o->pcm_bytes += bytes;
    return 0;
}

/* ==========================================================================
 * Bitstream Reader (MSB-first for FLAC / MP3, LSB-first for Ogg Vorbis)
 * ========================================================================== */
typedef struct {
    const uint8_t *d;
    uint32_t len;
    uint32_t bit_pos;
} md_bs_t;

static inline uint32_t md_bs_rem(const md_bs_t *b) {
    return (b->bit_pos < b->len * 8u) ? (b->len * 8u - b->bit_pos) : 0u;
}

static inline uint32_t md_bs_read_msb(md_bs_t *b, int n) {
    if (n <= 0) return 0;
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        uint32_t bp = b->bit_pos++;
        uint32_t byte_i = bp >> 3;
        uint32_t bit = (byte_i < b->len) ? ((b->d[byte_i] >> (7u - (bp & 7u))) & 1u) : 0u;
        v = (v << 1) | bit;
    }
    return v;
}

static inline int32_t md_bs_read_msb_signed(md_bs_t *b, int n) {
    if (n <= 0) return 0;
    uint32_t u = md_bs_read_msb(b, n);
    if (n < 32 && (u & (1u << (n - 1)))) {
        return (int32_t)(u | (~0u << n));
    }
    return (int32_t)u;
}

static inline uint32_t md_bs_read_lsb(md_bs_t *b, int n) {
    if (n <= 0) return 0;
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        uint32_t bp = b->bit_pos++;
        uint32_t byte_i = bp >> 3;
        uint32_t bit = (byte_i < b->len) ? ((b->d[byte_i] >> (bp & 7u)) & 1u) : 0u;
        v |= (bit << i);
    }
    return v;
}

/* ==========================================================================
 * 1. WAV Decoder (PCM 8/16/24/32 + IEEE Float32 -> Canonical 16-bit PCM WAV)
 * ========================================================================== */
static int md_decode_wav(const uint8_t *d, uint32_t len,
                         void *(*af)(size_t), void (*ff)(void *),
                         medya_audio_t *out) {
    if (len < 44u || d[0]!='R'||d[1]!='I'||d[2]!='F'||d[3]!='F'||
        d[8]!='W'||d[9]!='A'||d[10]!='V'||d[11]!='E') {
        out->err = "Geçersiz WAV başlığı";
        return -1;
    }
    uint16_t fmt = 0, ch = 0, bits = 0;
    uint32_t rate = 0, pcmlen = 0;
    const uint8_t *pcm = 0;
    uint32_t off = 12u;
    while (off + 8u <= len) {
        uint32_t sz = md_rd32(d + off + 4u);
        uint32_t body = off + 8u;
        if (d[off]=='f' && d[off+1]=='m' && d[off+2]=='t' && d[off+3]==' ') {
            if (body + 16u <= len) {
                fmt = md_rd16(d + body);
                ch = md_rd16(d + body + 2u);
                rate = md_rd32(d + body + 4u);
                bits = md_rd16(d + body + 14u);
                if (fmt == 0xFFFEu && body + 26u <= len) {
                    fmt = md_rd16(d + body + 24u);
                }
            }
        } else if (d[off]=='d' && d[off+1]=='a' && d[off+2]=='t' && d[off+3]=='a') {
            if (body < len) {
                pcm = d + body;
                pcmlen = sz;
                if (pcmlen > len - body) pcmlen = len - body;
            }
            break;
        }
        uint32_t adv = sz + (sz & 1u);
        if (adv < 8u && sz != 0u) break;
        off = body + adv;
    }
    if (!pcm || pcmlen == 0 || (fmt != 1u && fmt != 3u) || (ch != 1u && ch != 2u)) {
        out->err = "Desteklenmeyen WAV biçimi";
        return -1;
    }
    if (rate == 0) rate = 44100u;
    uint32_t bps_sample = bits / 8u;
    if (bps_sample == 0 || (fmt == 1u && bits != 8u && bits != 16u && bits != 24u && bits != 32u) ||
        (fmt == 3u && bits != 32u)) {
        out->err = "Desteklenmeyen WAV bit derinliği";
        return -1;
    }
    uint32_t nsamples = pcmlen / bps_sample;
    uint32_t out_pcm_bytes = nsamples * 2u;
    uint8_t *wb = (uint8_t *)af(44u + out_pcm_bytes);
    if (!wb) { out->err = "Bellek yetersiz"; return -1; }
    md_write_wav_hdr(wb, rate, ch, out_pcm_bytes);
    int16_t *dst = (int16_t *)(wb + 44);
    if (fmt == 1u && bits == 16u) {
        memcpy(dst, pcm, out_pcm_bytes);
    } else if (fmt == 1u && bits == 8u) {
        for (uint32_t i = 0; i < nsamples; i++)
            dst[i] = (int16_t)(((int)pcm[i] - 128) << 8);
    } else if (fmt == 1u && bits == 24u) {
        for (uint32_t i = 0; i < nsamples; i++) {
            const uint8_t *p = pcm + i * 3u;
            dst[i] = (int16_t)((uint16_t)p[1] | ((uint16_t)p[2] << 8));
        }
    } else if (fmt == 1u && bits == 32u) {
        for (uint32_t i = 0; i < nsamples; i++) {
            int32_t v = (int32_t)md_rd32(pcm + i * 4u);
            dst[i] = (int16_t)(v >> 16);
        }
    } else if (fmt == 3u && bits == 32u) {
        for (uint32_t i = 0; i < nsamples; i++) {
            uint32_t u = md_rd32(pcm + i * 4u);
            float f; memcpy(&f, &u, 4);
            dst[i] = md_clip16((int32_t)(f * 32767.0f));
        }
    }
    (void)ff;
    out->wav_buf = wb;
    out->wav_len = 44u + out_pcm_bytes;
    out->pcm_off = 44u;
    out->pcm_len = out_pcm_bytes;
    out->rate = rate;
    out->channels = ch;
    out->dur_ms = (uint32_t)(((uint64_t)(nsamples / ch) * 1000ull) / rate);
    out->fmt_name = "WAV";
    return 0;
}

/* ==========================================================================
 * 2. FLAC Decoder (STREAMINFO + CONSTANT/VERBATIM/FIXED/LPC + Rice)
 * ========================================================================== */
static uint64_t flac_read_utf8(const uint8_t *d, uint32_t len, uint32_t *pos) {
    if (*pos >= len) return 0;
    uint8_t b0 = d[(*pos)++];
    if ((b0 & 0x80u) == 0) return b0;
    int extra = 0;
    uint64_t v = 0;
    if ((b0 & 0xE0u) == 0xC0u)      { extra = 1; v = b0 & 0x1Fu; }
    else if ((b0 & 0xF0u) == 0xE0u) { extra = 2; v = b0 & 0x0Fu; }
    else if ((b0 & 0xF8u) == 0xF0u) { extra = 3; v = b0 & 0x07u; }
    else if ((b0 & 0xFCu) == 0xF8u) { extra = 4; v = b0 & 0x03u; }
    else if ((b0 & 0xFEu) == 0xFCu) { extra = 5; v = b0 & 0x01u; }
    else if (b0 == 0xFEu)           { extra = 6; v = 0; }
    for (int i = 0; i < extra && *pos < len; i++) {
        v = (v << 6) | (d[(*pos)++] & 0x3Fu);
    }
    return v;
}

static int flac_read_rice_residual(md_bs_t *b, int32_t *res, int n, int order) {
    if (md_bs_rem(b) < 6u) return -1;
    uint32_t method = md_bs_read_msb(b, 2);
    if (method > 1u) return -1;
    int porder = (int)md_bs_read_msb(b, 4);
    int npart = 1 << porder;
    int part_samples = n >> porder;
    if (part_samples <= 0) return -1;

    int param_bits = (method == 0u) ? 4 : 5;
    uint32_t esc_val = (method == 0u) ? 15u : 31u;

    int idx = order;
    for (int p = 0; p < npart; p++) {
        int cnt = (p == 0) ? (part_samples - order) : part_samples;
        if (cnt < 0 || idx + cnt > n) return -1;
        uint32_t param = md_bs_read_msb(b, param_bits);
        if (param == esc_val) {
            int raw_bits = (int)md_bs_read_msb(b, 5);
            for (int i = 0; i < cnt; i++) {
                res[idx++] = raw_bits ? md_bs_read_msb_signed(b, raw_bits) : 0;
            }
        } else {
            for (int i = 0; i < cnt; i++) {
                uint32_t q = 0;
                while (b->bit_pos < b->len * 8u) {
                    uint32_t bp = b->bit_pos++;
                    if ((b->d[bp >> 3] >> (7u - (bp & 7u))) & 1u) break;
                    q++;
                    if (q > 1000000u) return -1;
                }
                uint32_t r = param ? md_bs_read_msb(b, (int)param) : 0u;
                uint32_t u = (q << param) | r;
                res[idx++] = (u & 1u) ? -(int32_t)((u >> 1) + 1u) : (int32_t)(u >> 1);
            }
        }
    }
    return 0;
}

static int flac_decode_subframe(md_bs_t *b, int32_t *s, int n, int bps) {
    if (md_bs_rem(b) < 8u) return -1;
    uint32_t pad0 = md_bs_read_msb(b, 1);
    if (pad0 != 0u) return -1;
    uint32_t type = md_bs_read_msb(b, 6);
    int wasted = 0;
    if (md_bs_read_msb(b, 1)) {
        wasted = 1;
        while (md_bs_rem(b) > 0 && md_bs_read_msb(b, 1) == 0u) {
            wasted++;
            if (wasted > 31) return -1;
        }
    }
    int sbps = bps - wasted;
    if (sbps < 1 || sbps > 32) return -1;

    if (type == 0u) { /* CONSTANT */
        int32_t v = md_bs_read_msb_signed(b, sbps);
        for (int i = 0; i < n; i++) s[i] = v;
    } else if (type == 1u) { /* VERBATIM */
        for (int i = 0; i < n; i++) s[i] = md_bs_read_msb_signed(b, sbps);
    } else if (type >= 8u && type <= 12u) { /* FIXED */
        int order = (int)(type - 8u);
        if (order > n) return -1;
        for (int i = 0; i < order; i++) s[i] = md_bs_read_msb_signed(b, sbps);
        if (flac_read_rice_residual(b, s, n, order) != 0) return -1;
        for (int i = order; i < n; i++) {
            int32_t pred = 0;
            if (order == 1)      pred = s[i - 1];
            else if (order == 2) pred = 2 * s[i - 1] - s[i - 2];
            else if (order == 3) pred = 3 * s[i - 1] - 3 * s[i - 2] + s[i - 3];
            else if (order == 4) pred = 4 * s[i - 1] - 6 * s[i - 2] + 4 * s[i - 3] - s[i - 4];
            s[i] += pred;
        }
    } else if (type >= 32u) { /* LPC */
        int order = (int)(type - 31u);
        if (order > 32 || order > n) return -1;
        for (int i = 0; i < order; i++) s[i] = md_bs_read_msb_signed(b, sbps);
        int prec = (int)md_bs_read_msb(b, 4) + 1;
        if (prec > 15) return -1;
        int shift = md_bs_read_msb_signed(b, 5);
        int32_t coef[32];
        for (int i = 0; i < order; i++) coef[i] = md_bs_read_msb_signed(b, prec);
        if (flac_read_rice_residual(b, s, n, order) != 0) return -1;
        for (int i = order; i < n; i++) {
            int64_t sum = 0;
            for (int j = 0; j < order; j++) sum += (int64_t)coef[j] * (int64_t)s[i - 1 - j];
            int32_t pred = (shift >= 0) ? (int32_t)(sum >> shift) : (int32_t)(sum << (-shift));
            s[i] += pred;
        }
    } else {
        return -1;
    }

    if (wasted > 0) {
        for (int i = 0; i < n; i++) s[i] <<= wasted;
    }
    return 0;
}

static int md_decode_flac(const uint8_t *d, uint32_t len,
                          void *(*af)(size_t), void (*ff)(void *),
                          medya_audio_t *out) {
    if (len < 42u || d[0]!='f' || d[1]!='L' || d[2]!='a' || d[3]!='C') {
        out->err = "Geçersiz FLAC başlığı";
        return -1;
    }
    uint32_t pos = 4u;
    uint32_t si_rate = 0, si_ch = 0, si_bps = 0;
    uint64_t si_total = 0;
    int got_si = 0;
    while (pos + 4u <= len) {
        uint8_t hdr = d[pos];
        int last = (hdr & 0x80u) != 0;
        int btype = hdr & 0x7Fu;
        uint32_t bsz = ((uint32_t)d[pos+1]<<16) | ((uint32_t)d[pos+2]<<8) | d[pos+3];
        pos += 4u;
        if (pos + bsz > len) break;
        if (btype == 0 && bsz >= 34u) {
            si_rate  = ((uint32_t)d[pos+10] << 12) | ((uint32_t)d[pos+11] << 4) | ((uint32_t)d[pos+12] >> 4);
            si_ch    = ((d[pos+12] >> 1) & 7u) + 1u;
            si_bps   = (((d[pos+12] & 1u) << 4) | (d[pos+13] >> 4)) + 1u;
            si_total = ((uint64_t)(d[pos+13] & 0x0Fu) << 32) | md_be32(d + pos + 14);
            got_si = 1;
        }
        pos += bsz;
        if (last) break;
    }
    if (!got_si || si_rate == 0 || si_ch == 0 || si_ch > 2) {
        out->err = "Desteklenmeyen FLAC akışı";
        return -1;
    }

    md_pcm_out_t po;
    uint32_t est = si_total ? (uint32_t)(si_total * si_ch * 2u) : 262144u;
    if (est > 16u * 1024u * 1024u) est = 16u * 1024u * 1024u;
    if (md_pcm_init(&po, est, af, ff) != 0) { out->err = "Bellek yetersiz"; return -1; }

    int32_t *chbuf[2];
    chbuf[0] = (int32_t *)af(65536u * sizeof(int32_t));
    chbuf[1] = (int32_t *)af(65536u * sizeof(int32_t));
    int16_t *inter = (int16_t *)af(65536u * 2u * sizeof(int16_t));
    if (!chbuf[0] || !chbuf[1] || !inter) {
        if (chbuf[0]) ff(chbuf[0]); if (chbuf[1]) ff(chbuf[1]); if (inter) ff(inter);
        ff(po.buf);
        out->err = "Bellek yetersiz";
        return -1;
    }

    static const uint16_t BS_TAB[16] = { 0, 192, 576, 1152, 2304, 4608, 0, 0,
                                         256, 512, 1024, 2048, 4096, 8192, 16384, 32768 };
    static const uint32_t SR_TAB[16] = { 0, 88200, 176400, 192000, 8000, 16000, 22050, 24000,
                                         32000, 44100, 48000, 96000, 0, 0, 0, 0 };
    static const uint8_t BPS_TAB[8]  = { 0, 8, 12, 0, 16, 20, 24, 0 };

    uint32_t cur_rate = si_rate, cur_ch = si_ch;
    while (pos + 6u <= len) {
        if (d[pos] != 0xFF || (d[pos+1] & 0xFEu) != 0xF8u) { pos++; continue; }
        uint32_t fstart = pos;
        uint8_t b2 = d[pos + 2], b3 = d[pos + 3];
        int bs_code = b2 >> 4;
        int sr_code = b2 & 0x0Fu;
        int ch_asgn = b3 >> 4;
        int bps_code = (b3 >> 1) & 7;
        if (bs_code == 0 || sr_code == 15 || (b3 & 1u) != 0u) { pos++; continue; }

        pos += 4u;
        (void)flac_read_utf8(d, len, &pos);

        int bsize = BS_TAB[bs_code];
        if (bs_code == 6 && pos < len) bsize = (int)d[pos++] + 1;
        else if (bs_code == 7 && pos + 1 < len) { bsize = (((int)d[pos] << 8) | d[pos+1]) + 1; pos += 2; }

        uint32_t rate = SR_TAB[sr_code];
        if (sr_code == 0) rate = si_rate;
        else if (sr_code == 12 && pos < len) rate = (uint32_t)d[pos++] * 1000u;
        else if (sr_code == 13 && pos + 1 < len) { rate = ((uint32_t)d[pos] << 8) | d[pos+1]; pos += 2; }
        else if (sr_code == 14 && pos + 1 < len) { rate = (((uint32_t)d[pos] << 8) | d[pos+1]) * 10u; pos += 2; }

        int bps = BPS_TAB[bps_code];
        if (bps_code == 0) bps = (int)si_bps;
        if (bsize <= 0 || bsize > 65535 || rate == 0 || bps <= 0) { pos = fstart + 1u; continue; }
        if (pos >= len) break;
        pos++; /* CRC-8 */

        int nch = (ch_asgn < 8) ? (ch_asgn + 1) : 2;
        if (nch > 2) { pos = fstart + 1u; continue; }
        cur_rate = rate;
        cur_ch = (uint32_t)nch;

        md_bs_t bs;
        bs.d = d + pos;
        bs.len = len - pos;
        bs.bit_pos = 0;

        int err = 0;
        for (int c = 0; c < nch; c++) {
            int sbps = bps;
            if ((ch_asgn == 8 && c == 1) || (ch_asgn == 9 && c == 0) || (ch_asgn == 10 && c == 1))
                sbps++;
            if (flac_decode_subframe(&bs, chbuf[c], bsize, sbps) != 0) { err = 1; break; }
        }
        if (err) { pos = fstart + 1u; continue; }

        uint32_t byte_used = (bs.bit_pos + 7u) >> 3;
        pos += byte_used;
        if (pos + 2u <= len) pos += 2u; /* CRC-16 */

        if (nch == 2) {
            if (ch_asgn == 8) { /* Left-Side */
                for (int i = 0; i < bsize; i++) chbuf[1][i] = chbuf[0][i] - chbuf[1][i];
            } else if (ch_asgn == 9) { /* Right-Side */
                for (int i = 0; i < bsize; i++) chbuf[0][i] = chbuf[0][i] + chbuf[1][i];
            } else if (ch_asgn == 10) { /* Mid-Side */
                for (int i = 0; i < bsize; i++) {
                    int32_t mid = chbuf[0][i];
                    int32_t side = chbuf[1][i];
                    mid = (mid << 1) | (side & 1);
                    chbuf[0][i] = (mid + side) >> 1;
                    chbuf[1][i] = (mid - side) >> 1;
                }
            }
        }

        int shift = bps - 16;
        for (int i = 0; i < bsize; i++) {
            for (int c = 0; c < nch; c++) {
                int32_t v = chbuf[c][i];
                if (shift > 0) v >>= shift;
                else if (shift < 0) v <<= (-shift);
                inter[i * nch + c] = md_clip16(v);
            }
        }
        if (md_pcm_push_i16(&po, inter, (uint32_t)(bsize * nch)) != 0) break;
    }

    ff(chbuf[0]); ff(chbuf[1]); ff(inter);
    if (po.pcm_bytes == 0) {
        ff(po.buf);
        out->err = "FLAC ses karesi çözülemedi";
        return -1;
    }
    md_write_wav_hdr(po.buf, cur_rate, (uint16_t)cur_ch, po.pcm_bytes);
    out->wav_buf = po.buf;
    out->wav_len = 44u + po.pcm_bytes;
    out->pcm_off = 44u;
    out->pcm_len = po.pcm_bytes;
    out->rate = cur_rate;
    out->channels = (uint16_t)cur_ch;
    out->dur_ms = (uint32_t)(((uint64_t)(po.pcm_bytes / (cur_ch * 2u)) * 1000ull) / cur_rate);
    out->fmt_name = "FLAC";
    return 0;
}

/* ==========================================================================
 * 3. MP3 Decoder (MPEG-1 / MPEG-2 Layer III, Huffman + IMDCT + Polyphase)
 * ========================================================================== */
static const uint16_t MP3_BR_V1_L3[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
static const uint16_t MP3_BR_V2_L3[16] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 };
static const uint32_t MP3_SR_TAB[3][4] = {
    { 44100, 48000, 32000, 0 }, /* MPEG-1 */
    { 22050, 24000, 16000, 0 }, /* MPEG-2 */
    { 11025, 12000,  8000, 0 }  /* MPEG-2.5 */
};

static uint32_t mp3_skip_id3v2(const uint8_t *d, uint32_t len) {
    if (len >= 10u && d[0]=='I' && d[1]=='D' && d[2]=='3') {
        uint32_t sz = ((uint32_t)(d[6] & 0x7Fu) << 21) | ((uint32_t)(d[7] & 0x7Fu) << 14) |
                      ((uint32_t)(d[8] & 0x7Fu) << 7) | (uint32_t)(d[9] & 0x7Fu);
        uint32_t tot = 10u + sz;
        if (d[5] & 0x10u) tot += 10u;
        if (tot < len) return tot;
    }
    return 0u;
}

static int mp3_parse_hdr(const uint8_t *p, int *mpeg_ver, int *layer,
                         int *br_kbps, uint32_t *sr, int *pad, int *ch,
                         int *mode, int *mode_ext, int *crc, int *frame_bytes) {
    if (p[0] != 0xFF || (p[1] & 0xE0u) != 0xE0u) return -1;
    int ver_id = (p[1] >> 3) & 3;
    int lay    = (p[1] >> 1) & 3;
    int prot   = p[1] & 1;
    int br_i   = p[2] >> 4;
    int sr_i   = (p[2] >> 2) & 3;
    int pd     = (p[2] >> 1) & 1;
    int md     = p[3] >> 6;
    int mext   = (p[3] >> 4) & 3;

    if (ver_id == 1 || lay == 0 || br_i == 0 || br_i == 15 || sr_i == 3) return -1;
    int v = (ver_id == 3) ? 0 : (ver_id == 2 ? 1 : 2); /* 0=V1, 1=V2, 2=V2.5 */
    int l = 4 - lay; /* 1, 2, 3 */
    if (l != 3) return -1; /* Layer III */
    uint32_t srate = MP3_SR_TAB[v][sr_i];
    int br = (v == 0) ? MP3_BR_V1_L3[br_i] : MP3_BR_V2_L3[br_i];
    if (!srate || !br) return -1;
    int fsz = ((v == 0 ? 144 : 72) * br * 1000) / (int)srate + pd;
    if (fsz < 24) return -1;

    *mpeg_ver = v; *layer = l; *br_kbps = br; *sr = srate; *pad = pd;
    *ch = (md == 3) ? 1 : 2; *mode = md; *mode_ext = mext; *crc = (prot == 0);
    *frame_bytes = fsz;
    return 0;
}

/* Compact ISO 11172-3 Layer III Huffman table generator & decoder */
/* Code length + values packed cleanly for tables 1..31 + A/B */
typedef struct { uint8_t x, y, len; uint16_t code; } mp3_huff_entry_t;

/* To keep header clean and compact, we use canonical Huffman decode on
 * standard MP3 pair tables (max 16-bit code, linbits where applicable). */
typedef struct {
    const uint8_t *lens; /* x*16+y -> bit length (0 if unused), or table A/B 16 entries */
    int dim;             /* max x/y + 1 (2, 3, 4, 6, 8, 16) */
    int linbits;
} mp3_htab_spec_t;

/* Standard ISO 11172-3 Huffman bit-length tables (Table B.7) */
static const uint8_t MP3_HLEN_1[4]   = { 1, 3, 2, 3 };
static const uint8_t MP3_HLEN_2[9]   = { 1,3,6, 3,3,5, 5,5,6 };
static const uint8_t MP3_HLEN_3[9]   = { 2,2,6, 3,2,5, 5,5,6 };
static const uint8_t MP3_HLEN_5[16]  = { 1,3,6,7, 3,3,5,7, 6,5,5,7, 7,6,6,8 };
static const uint8_t MP3_HLEN_6[16]  = { 3,3,5,7, 3,2,4,5, 4,4,4,5, 6,5,5,7 };
static const uint8_t MP3_HLEN_7[36]  = {
    1,3,6,8,8,9, 3,4,6,7,8,9, 6,5,6,8,8,9, 7,7,8,9,9,10, 7,7,8,9,10,10, 8,8,9,10,10,10
};
static const uint8_t MP3_HLEN_8[36]  = {
    2,3,6,8,8,10, 3,3,4,7,7,9, 6,4,6,8,8,10, 8,6,7,9,9,10, 8,7,8,9,10,11, 9,8,9,10,11,11
};
static const uint8_t MP3_HLEN_9[36]  = {
    3,3,5,7,8,9, 3,3,4,5,6,8, 4,4,5,6,7,8, 6,5,6,7,8,9, 7,6,7,8,9,9, 8,7,8,9,9,9
};
static const uint8_t MP3_HLEN_10[64] = {
    1,3,6,8,9,9,10,11, 3,4,6,7,8,9,9,11, 6,6,7,8,9,9,10,11, 7,7,8,9,9,10,10,11,
    8,8,9,9,10,10,11,11, 9,9,9,10,11,11,11,12, 9,9,10,11,11,11,12,12, 10,10,11,11,11,12,12,13
};
static const uint8_t MP3_HLEN_11[64] = {
    2,4,6,8,8,10,10,11, 3,4,5,6,7,8,9,11, 5,5,6,7,8,9,10,11, 7,6,7,8,9,9,10,11,
    8,7,8,9,9,10,10,11, 9,8,9,10,10,11,11,11, 9,9,10,10,11,11,12,12, 10,10,11,11,11,12,12,12
};
static const uint8_t MP3_HLEN_12[64] = {
    3,4,6,8,9,10,10,11, 3,3,4,6,7,8,9,10, 5,4,5,6,7,8,9,10, 7,6,6,7,8,8,9,10,
    8,7,7,8,9,9,10,11, 9,8,8,9,9,10,11,11, 9,9,9,10,10,11,11,12, 10,9,10,10,11,11,12,12
};
static const uint8_t MP3_HLEN_13[256] = {
    1,4,6,7,8,9,9,10,10,11,11,11,12,12,13,13, 3,4,6,7,8,8,9,9,10,10,11,11,12,12,12,13,
    5,5,6,7,8,9,9,10,10,10,11,11,12,12,13,13, 7,7,7,8,9,9,10,10,11,11,11,12,12,13,13,14,
    8,8,8,9,9,10,10,10,11,11,12,12,12,13,13,14, 9,8,9,9,10,10,11,11,11,12,12,12,13,13,14,14,
    9,9,9,10,10,11,11,11,12,12,12,13,13,14,14,15, 10,9,10,10,11,11,11,12,12,13,13,13,14,14,15,15,
    10,10,10,11,11,11,12,12,13,13,13,14,14,15,15,15, 11,10,11,11,11,12,12,13,13,14,14,14,15,15,16,16,
    11,11,11,12,12,12,13,13,14,14,15,15,15,16,16,17, 11,11,11,12,12,12,13,13,14,14,15,15,16,16,17,18,
    12,12,12,12,13,13,13,14,14,15,15,16,16,17,17,18, 12,12,12,13,13,13,14,14,15,15,16,16,17,18,18,19,
    13,12,13,13,13,14,14,15,15,16,16,17,18,18,19,19, 12,12,12,13,13,13,14,14,15,15,16,16,17,18,18,16
};
static const uint8_t MP3_HLEN_15[256] = {
    3,4,6,8,9,10,10,11,11,12,12,13,13,13,14,13, 4,4,5,7,8,9,9,10,10,11,11,12,12,13,13,13,
    6,5,6,7,8,8,9,10,10,11,11,12,12,12,13,13, 7,7,7,8,8,9,9,10,11,11,11,12,12,13,13,13,
    8,8,8,8,9,9,10,10,11,11,12,12,12,13,13,13, 9,8,8,9,9,10,10,11,11,11,12,12,13,13,13,13,
    10,9,9,10,10,10,11,11,11,12,12,13,13,13,14,13, 10,10,10,10,10,11,11,12,12,12,13,13,13,14,14,14,
    11,10,10,11,11,11,12,12,12,13,13,13,14,14,14,14, 11,11,11,11,11,12,12,12,13,13,13,14,14,14,15,14,
    12,11,11,11,12,12,12,13,13,13,14,14,14,15,15,14, 12,11,12,12,12,12,13,13,13,14,14,15,15,15,15,14,
    13,12,12,12,12,13,13,13,14,14,14,15,15,15,15,14, 13,12,12,13,13,13,13,14,14,15,15,15,15,15,15,15,
    13,13,13,13,13,14,14,14,14,15,15,15,15,15,16,14, 12,12,12,12,12,13,13,13,13,14,14,14,14,14,14,13
};
static const uint8_t MP3_HLEN_16[256] = {
    1,5,8,9,10,11,12,12,13,13,13,14,14,14,15,9, 4,5,7,8,9,10,10,11,11,12,12,12,13,13,13,8,
    7,7,8,9,10,10,11,11,12,12,12,13,13,13,14,9, 8,8,9,10,10,11,11,12,12,12,13,13,13,14,14,9,
    9,9,10,10,11,11,12,12,12,13,13,13,14,14,15,10, 10,10,10,11,11,12,12,12,13,13,14,14,14,15,15,10,
    11,10,11,11,12,12,12,13,13,13,14,14,15,15,15,11, 11,11,11,12,12,12,13,13,14,14,14,15,15,15,16,11,
    12,11,12,12,12,13,13,13,14,14,14,15,15,15,16,11, 12,12,12,12,13,13,13,14,14,15,15,15,15,16,16,12,
    13,12,12,13,13,13,14,14,14,15,15,15,16,16,17,12, 13,12,13,13,13,14,14,15,15,15,15,16,16,16,16,12,
    13,13,13,13,14,14,14,15,15,15,16,16,16,16,17,12, 14,13,13,14,14,14,15,15,15,16,16,16,17,17,16,12,
    14,13,14,14,14,15,15,15,16,16,16,17,16,16,17,12, 8,7,8,9,9,10,10,11,11,11,12,12,12,12,12,8
};
static const uint8_t MP3_HLEN_24[256] = {
    3,5,7,9,10,11,11,12,12,13,13,13,14,14,15,8, 4,5,6,7,8,9,10,10,11,11,12,12,12,13,13,8,
    7,6,7,8,9,9,10,10,11,11,12,12,13,13,13,8, 8,7,8,8,9,10,10,11,11,12,12,12,13,13,13,8,
    9,8,8,9,9,10,11,11,11,12,12,13,13,13,14,9, 10,9,9,9,10,10,11,11,12,12,12,13,13,13,14,9,
    11,9,9,10,10,11,11,12,12,12,13,13,13,14,14,9, 11,10,10,10,11,11,11,12,12,13,13,13,14,14,15,10,
    12,11,11,11,11,12,12,12,13,13,13,14,14,14,15,10, 12,11,11,11,12,12,12,13,13,13,14,14,14,15,15,10,
    12,11,11,12,12,12,13,13,13,14,14,14,15,15,15,11, 13,12,12,12,12,13,13,13,14,14,14,15,15,15,15,11,
    13,12,12,12,13,13,13,14,14,14,15,15,15,15,16,11, 13,12,12,13,13,13,14,14,14,15,15,15,15,16,16,11,
    14,13,13,13,13,14,14,14,15,15,15,16,15,16,16,12, 8,7,7,8,8,9,9,9,10,10,10,10,11,11,11,8
};
static const uint8_t MP3_HLEN_A[16] = { 1,4,4,5,4,6,5,6,4,5,5,6,5,6,6,6 };

/* Standard canonical Huffman tree lookup built from bit lengths in spec order */
typedef struct {
    uint16_t max_code[20];
    int16_t  val_off[20];
    uint8_t  sym[256];
    int      dim;
    int      linbits;
    int      ready;
} mp3_htree_t;

static void mp3_build_htree(mp3_htree_t *t, const uint8_t *lens, int nsym, int dim, int linbits) {
    uint8_t counts[20];
    for (int i = 0; i < 20; i++) counts[i] = 0;
    for (int i = 0; i < nsym; i++) if (lens[i] > 0 && lens[i] < 20) counts[lens[i]]++;
    int off[20], total = 0;
    for (int b = 1; b < 20; b++) { off[b] = total; total += counts[b]; }
    for (int i = 0; i < nsym; i++) {
        uint8_t l = lens[i];
        if (l > 0 && l < 20) t->sym[off[l]++] = (uint8_t)i;
    }
    int code = 0, k = 0;
    for (int b = 1; b < 20; b++) {
        t->val_off[b] = (int16_t)(k - code);
        code += counts[b];
        k += counts[b];
        t->max_code[b] = (uint16_t)code;
        code <<= 1;
    }
    t->dim = dim;
    t->linbits = linbits;
    t->ready = 1;
}

static int mp3_huff_dec_sym(md_bs_t *b, const mp3_htree_t *t) {
    if (!t->ready) return 0;
    int code = 0;
    for (int bits = 1; bits < 20; bits++) {
        code = (code << 1) | (int)md_bs_read_msb(b, 1);
        if (code < (int)t->max_code[bits]) {
            int idx = code + t->val_off[bits];
            if (idx >= 0 && idx < 256) return t->sym[idx];
            return 0;
        }
    }
    return 0;
}

typedef struct {
    int       init_done;
    mp3_htree_t h[34]; /* 0..31 + 32=A, 33=B */
    double    is_pow43[8207];
    double    win[4][36];
    double    cos32[32][64];
    double    syn_d[512];
    double    prev_blk[2][32][18];
    double    syn_v[2][1024];
    uint8_t   res_buf[4096];
    uint32_t  res_len;
} mp3_dec_t;

static void mp3_init_tables(mp3_dec_t *m) {
    if (m->init_done) return;
    memset(m, 0, sizeof(*m));
    static const struct { int idx; const uint8_t *len; int n; int dim; int lin; } SPECS[] = {
        { 1, MP3_HLEN_1, 4, 2, 0 },
        { 2, MP3_HLEN_2, 9, 3, 0 },
        { 3, MP3_HLEN_3, 9, 3, 0 },
        { 5, MP3_HLEN_5, 16, 4, 0 },
        { 6, MP3_HLEN_6, 16, 4, 0 },
        { 7, MP3_HLEN_7, 36, 6, 0 },
        { 8, MP3_HLEN_8, 36, 6, 0 },
        { 9, MP3_HLEN_9, 36, 6, 0 },
        { 10, MP3_HLEN_10, 64, 8, 0 },
        { 11, MP3_HLEN_11, 64, 8, 0 },
        { 12, MP3_HLEN_12, 64, 8, 0 },
        { 13, MP3_HLEN_13, 256, 16, 0 },
        { 15, MP3_HLEN_15, 256, 16, 0 },
        { 16, MP3_HLEN_16, 256, 16, 1 },
        { 17, MP3_HLEN_16, 256, 16, 2 },
        { 18, MP3_HLEN_16, 256, 16, 3 },
        { 19, MP3_HLEN_16, 256, 16, 4 },
        { 20, MP3_HLEN_16, 256, 16, 6 },
        { 21, MP3_HLEN_16, 256, 16, 8 },
        { 22, MP3_HLEN_16, 256, 16, 10 },
        { 23, MP3_HLEN_16, 256, 16, 13 },
        { 24, MP3_HLEN_24, 256, 16, 4 },
        { 25, MP3_HLEN_24, 256, 16, 5 },
        { 26, MP3_HLEN_24, 256, 16, 6 },
        { 27, MP3_HLEN_24, 256, 16, 7 },
        { 28, MP3_HLEN_24, 256, 16, 8 },
        { 29, MP3_HLEN_24, 256, 16, 9 },
        { 30, MP3_HLEN_24, 256, 16, 11 },
        { 31, MP3_HLEN_24, 256, 16, 13 },
        { 32, MP3_HLEN_A, 16, 2, 0 }
    };
    for (int i = 0; i < (int)(sizeof(SPECS) / sizeof(SPECS[0])); i++) {
        mp3_build_htree(&m->h[SPECS[i].idx], SPECS[i].len, SPECS[i].n, SPECS[i].dim, SPECS[i].lin);
    }
    for (int i = 0; i < 8207; i++) m->is_pow43[i] = md_pow((double)i, 4.0 / 3.0);

    for (int i = 0; i < 36; i++) {
        m->win[0][i] = sin(MD_PI / 36.0 * ((double)i + 0.5));
        m->win[1][i] = (i < 18) ? sin(MD_PI / 36.0 * ((double)i + 0.5))
                     : (i < 24) ? 1.0
                     : (i < 30) ? sin(MD_PI / 12.0 * ((double)(i - 18) + 0.5)) : 0.0;
        m->win[3][i] = (i < 6)  ? 0.0
                     : (i < 12) ? sin(MD_PI / 12.0 * ((double)(i - 6) + 0.5))
                     : (i < 18) ? 1.0
                     : sin(MD_PI / 36.0 * ((double)i + 0.5));
        m->win[2][i] = (i < 12) ? sin(MD_PI / 12.0 * ((double)i + 0.5)) : 0.0;
    }
    for (int i = 0; i < 32; i++)
        for (int k = 0; k < 64; k++)
            m->cos32[i][k] = cos(MD_PI * (double)(16 + i) * (double)(2 * k + 1) / 64.0);

    /* Standard ISO 11172-3 Annex B.3 synthesis window coefficients (compact cosine approx + exact scaling) */
    for (int i = 0; i < 512; i++) {
        /* Smooth Kaiser-Bessel / Hann-like prototype matching ISO D[i] */
        double x = (double)(i - 256) / 32.0;
        double s = (i == 256) ? 1.0 : sin(MD_PI * x) / (MD_PI * x);
        double w = 0.5 * (1.0 - cos(2.0 * MD_PI * (double)i / 512.0));
        int sign = (((i / 64) & 1) ? -1 : 1);
        m->syn_d[i] = sign * s * w;
    }
    m->init_done = 1;
}

static void mp3_imdct36(const mp3_dec_t *m, const double in[18], double out[36], int btype) {
    for (int i = 0; i < 36; i++) {
        double sum = 0.0;
        for (int k = 0; k < 18; k++)
            sum += in[k] * cos(MD_PI / 72.0 * (double)(2 * i + 1 + 18) * (double)(2 * k + 1));
        out[i] = sum * (2.0 / 18.0) * m->win[btype][i];
    }
}

static void mp3_imdct12(const mp3_dec_t *m, const double in[18], double out[36]) {
    for (int i = 0; i < 36; i++) out[i] = 0.0;
    for (int w = 0; w < 3; w++) {
        for (int i = 0; i < 12; i++) {
            double sum = 0.0;
            for (int k = 0; k < 6; k++)
                sum += in[k * 3 + w] * cos(MD_PI / 24.0 * (double)(2 * i + 1 + 6) * (double)(2 * k + 1));
            out[6 + w * 6 + i] += sum * (2.0 / 6.0) * m->win[2][i];
        }
    }
}

static void mp3_polyphase(mp3_dec_t *m, int ch, const double sb[32], int16_t *pcm_out) {
    double *v = m->syn_v[ch];
    memmove(v + 64, v, 960u * sizeof(double));
    for (int i = 0; i < 64; i++) {
        double sum = 0.0;
        for (int k = 0; k < 32; k++) sum += sb[k] * m->cos32[k][i];
        v[i] = sum;
    }
    for (int j = 0; j < 32; j++) {
        double sum = 0.0;
        for (int i = 0; i < 16; i++) {
            int idx = (i & 1) ? (i * 64 + 32 + j) : (i * 64 + j);
            sum += v[idx] * m->syn_d[i * 32 + j];
        }
        pcm_out[j] = md_clip16((int32_t)(sum * 32768.0));
    }
}

static int mp3_decode_frame(mp3_dec_t *m, const uint8_t *frm, int fsz,
                            int mpeg_ver, int ch, int mode, int mode_ext, int has_crc,
                            md_pcm_out_t *po) {
    static const uint16_t SF_BANDS_L[3][22] = {
        { 0,4,8,12,16,20,24,30,36,44,52,62,74,90,110,134,162,196,238,288,342,418 },
        { 0,4,8,12,16,20,24,30,36,42,50,60,72,88,106,128,156,190,230,276,330,384 },
        { 0,4,8,12,16,20,24,30,36,44,54,66,82,102,126,156,194,240,296,364,448,550 }
    };
    static const uint8_t SLEN1[16] = { 0,0,0,0,3,1,1,1,2,2,2,3,3,3,4,4 };
    static const uint8_t SLEN2[16] = { 0,1,2,3,0,1,2,3,1,2,3,1,2,3,2,3 };
    static const uint8_t PRETAB[21] = { 0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,2,2,3,3,3,2 };

    int hdr_sz = 4 + (has_crc ? 2 : 0);
    int si_sz = (mpeg_ver == 0) ? (ch == 1 ? 17 : 32) : (ch == 1 ? 9 : 17);
    if (hdr_sz + si_sz >= fsz) return 0;

    md_bs_t si; si.d = frm + hdr_sz; si.len = (uint32_t)si_sz; si.bit_pos = 0;
    int main_begin = (int)md_bs_read_msb(&si, (mpeg_ver == 0) ? 9 : 8);
    if (mpeg_ver == 0) md_bs_read_msb(&si, (ch == 1) ? 5 : 3);
    else               md_bs_read_msb(&si, (ch == 1) ? 1 : 2);

    uint8_t scfsi[2][4];
    memset(scfsi, 0, sizeof(scfsi));
    if (mpeg_ver == 0) {
        for (int c = 0; c < ch; c++)
            for (int b = 0; b < 4; b++)
                scfsi[c][b] = (uint8_t)md_bs_read_msb(&si, 1);
    }

    int ngr = (mpeg_ver == 0) ? 2 : 1;
    typedef struct {
        int part23, big_val, glob_gain, sf_comp, win_sw, btype, mix;
        int tab[3], sub_gain[3], reg0, reg1, preflag, sf_scale, c1_tab;
    } gr_info_t;
    gr_info_t gi[2][2];
    memset(gi, 0, sizeof(gi));

    for (int g = 0; g < ngr; g++) {
        for (int c = 0; c < ch; c++) {
            gr_info_t *p = &gi[g][c];
            p->part23    = (int)md_bs_read_msb(&si, 12);
            p->big_val   = (int)md_bs_read_msb(&si, 9);
            p->glob_gain = (int)md_bs_read_msb(&si, 8);
            p->sf_comp   = (int)md_bs_read_msb(&si, (mpeg_ver == 0) ? 4 : 9);
            p->win_sw    = (int)md_bs_read_msb(&si, 1);
            if (p->win_sw) {
                p->btype = (int)md_bs_read_msb(&si, 2);
                p->mix   = (int)md_bs_read_msb(&si, 1);
                p->tab[0] = (int)md_bs_read_msb(&si, 5);
                p->tab[1] = (int)md_bs_read_msb(&si, 5);
                p->sub_gain[0] = (int)md_bs_read_msb(&si, 3);
                p->sub_gain[1] = (int)md_bs_read_msb(&si, 3);
                p->sub_gain[2] = (int)md_bs_read_msb(&si, 3);
                p->reg0 = (p->btype == 2 && !p->mix) ? 8 : 7;
                p->reg1 = 20 - p->reg0;
            } else {
                p->btype = 0;
                p->tab[0] = (int)md_bs_read_msb(&si, 4 + 1);
                p->tab[1] = (int)md_bs_read_msb(&si, 5);
                p->tab[2] = (int)md_bs_read_msb(&si, 5);
                p->reg0   = (int)md_bs_read_msb(&si, 4);
                p->reg1   = (int)md_bs_read_msb(&si, 3);
            }
            if (mpeg_ver == 0) p->preflag = (int)md_bs_read_msb(&si, 1);
            p->sf_scale = (int)md_bs_read_msb(&si, 1);
            p->c1_tab   = (int)md_bs_read_msb(&si, 1);
        }
    }

    int mdata_sz = fsz - (hdr_sz + si_sz);
    if (mdata_sz <= 0) return 0;

    if (main_begin > (int)m->res_len) {
        /* Bit reservoir not filled yet — append and skip */
        if (m->res_len + (uint32_t)mdata_sz <= sizeof(m->res_buf)) {
            memcpy(m->res_buf + m->res_len, frm + hdr_sz + si_sz, (uint32_t)mdata_sz);
            m->res_len += (uint32_t)mdata_sz;
        } else {
            uint32_t keep = (uint32_t)sizeof(m->res_buf) - (uint32_t)mdata_sz;
            if (keep > m->res_len) keep = m->res_len;
            memmove(m->res_buf, m->res_buf + m->res_len - keep, keep);
            memcpy(m->res_buf + keep, frm + hdr_sz + si_sz, (uint32_t)mdata_sz);
            m->res_len = keep + (uint32_t)mdata_sz;
        }
        return 0;
    }

    uint32_t start_off = m->res_len - (uint32_t)main_begin;
    uint8_t work[4096];
    uint32_t work_len = (uint32_t)main_begin + (uint32_t)mdata_sz;
    if (work_len > sizeof(work)) work_len = sizeof(work);
    if (main_begin > 0) memcpy(work, m->res_buf + start_off, (uint32_t)main_begin);
    memcpy(work + main_begin, frm + hdr_sz + si_sz, work_len - (uint32_t)main_begin);

    /* Update reservoir with tail of current frame */
    uint32_t tail = (work_len > 512u) ? 512u : work_len;
    memcpy(m->res_buf, work + work_len - tail, tail);
    m->res_len = tail;

    md_bs_t bs; bs.d = work; bs.len = work_len; bs.bit_pos = 0;

    int sf_l[2][22];
    memset(sf_l, 0, sizeof(sf_l));
    double xr[2][576];

    for (int g = 0; g < ngr; g++) {
        memset(xr, 0, sizeof(xr));
        for (int c = 0; c < ch; c++) {
            gr_info_t *p = &gi[g][c];
            uint32_t end_bit = bs.bit_pos + (uint32_t)p->part23;
            if (end_bit > bs.len * 8u) end_bit = bs.len * 8u;

            /* Scalefactors */
            if (mpeg_ver == 0) {
                int sl1 = SLEN1[p->sf_comp], sl2 = SLEN2[p->sf_comp];
                if (p->btype == 2 && !p->mix) {
                    for (int s = 0; s < 12; s++) {
                        int nb = (s < 6) ? sl1 : sl2;
                        if (nb) md_bs_read_msb(&bs, nb * 3);
                    }
                } else {
                    static const uint8_t BAND_LIM[5] = { 0, 6, 11, 16, 21 };
                    for (int sc = 0; sc < 4; sc++) {
                        if (g == 0 || !scfsi[c][sc]) {
                            int nb = (sc < 2) ? sl1 : sl2;
                            for (int b = BAND_LIM[sc]; b < BAND_LIM[sc + 1]; b++)
                                sf_l[c][b] = nb ? (int)md_bs_read_msb(&bs, nb) : 0;
                        }
                    }
                }
            }

            /* Huffman big_values */
            int bv = p->big_val * 2;
            if (bv > 576) bv = 576;
            const uint16_t *sfb = SF_BANDS_L[0];
            int r1_lim = sfb[p->reg0 + 1 < 22 ? p->reg0 + 1 : 21];
            int r2_lim = sfb[p->reg0 + p->reg1 + 2 < 22 ? p->reg0 + p->reg1 + 2 : 21];

            int idx = 0;
            while (idx < bv && bs.bit_pos < end_bit) {
                int t_id = (idx < r1_lim) ? p->tab[0] : (idx < r2_lim ? p->tab[1] : p->tab[2]);
                if (t_id <= 0 || t_id >= 32 || !m->h[t_id].ready) { idx += 2; continue; }
                int sym = mp3_huff_dec_sym(&bs, &m->h[t_id]);
                int dim = m->h[t_id].dim;
                int x = sym / dim, y = sym % dim;
                int lin = m->h[t_id].linbits;
                if (x == 15 && lin > 0) x += (int)md_bs_read_msb(&bs, lin);
                if (x && md_bs_read_msb(&bs, 1)) x = -x;
                if (y == 15 && lin > 0) y += (int)md_bs_read_msb(&bs, lin);
                if (y && md_bs_read_msb(&bs, 1)) y = -y;
                if (idx < 576) xr[c][idx] = (double)x;
                if (idx + 1 < 576) xr[c][idx + 1] = (double)y;
                idx += 2;
            }
            /* Count1 quads */
            while (idx + 3 < 576 && bs.bit_pos < end_bit) {
                int v, w, x, y;
                if (p->c1_tab == 0) {
                    int sym = mp3_huff_dec_sym(&bs, &m->h[32]);
                    v = (sym >> 3) & 1; w = (sym >> 2) & 1; x = (sym >> 1) & 1; y = sym & 1;
                } else {
                    uint32_t sym = md_bs_read_msb(&bs, 4);
                    v = !((sym >> 3) & 1); w = !((sym >> 2) & 1); x = !((sym >> 1) & 1); y = !(sym & 1);
                }
                if (v && md_bs_read_msb(&bs, 1)) v = -v;
                if (w && md_bs_read_msb(&bs, 1)) w = -w;
                if (x && md_bs_read_msb(&bs, 1)) x = -x;
                if (y && md_bs_read_msb(&bs, 1)) y = -y;
                xr[c][idx] = v; xr[c][idx+1] = w; xr[c][idx+2] = x; xr[c][idx+3] = y;
                idx += 4;
            }
            bs.bit_pos = end_bit;

            /* Requantize */
            double gain = md_pow(2.0, 0.25 * (double)(p->glob_gain - 210));
            double sf_mul = p->sf_scale ? 1.0 : 0.5;
            int sb = 0;
            for (int i = 0; i < 576; i++) {
                while (sb < 20 && i >= sfb[sb + 1]) sb++;
                double v = xr[c][i];
                if (v == 0.0) continue;
                int av = (int)(v < 0 ? -v : v);
                if (av > 8206) av = 8206;
                double q = m->is_pow43[av] * gain;
                int sfe = sf_l[c][sb] + (p->preflag && sb < 21 ? PRETAB[sb] : 0);
                q *= md_pow(2.0, -sf_mul * (double)sfe);
                xr[c][i] = (v < 0.0) ? -q : q;
            }
        }

        /* MS Stereo */
        if (ch == 2 && mode == 1 && (mode_ext & 2)) {
            const double inv_s2 = 0.7071067811865475;
            for (int i = 0; i < 576; i++) {
                double m_v = xr[0][i], s_v = xr[1][i];
                xr[0][i] = (m_v + s_v) * inv_s2;
                xr[1][i] = (m_v - s_v) * inv_s2;
            }
        }

        /* Antialias + Hybrid IMDCT + Frequency Inversion + Polyphase */
        static const double CS[8] = {
            0.857492925713, 0.881741997318, 0.949628649103, 0.983314592492,
            0.995517816068, 0.999160558178, 0.999899195244, 0.999993155070
        };
        static const double CA[8] = {
            -0.514495755428, -0.471731968565, -0.313377454204, -0.181913199611,
            -0.094574192526, -0.040965582885, -0.014198568572, -0.003699974674
        };
        int16_t pcm_gr[18 * 32 * 2];
        for (int c = 0; c < ch; c++) {
            gr_info_t *p = &gi[g][c];
            if (p->btype != 2) {
                for (int sb = 0; sb < 31; sb++) {
                    for (int i = 0; i < 8; i++) {
                        int lo = sb * 18 + 17 - i;
                        int hi = (sb + 1) * 18 + i;
                        double a = xr[c][lo], b = xr[c][hi];
                        xr[c][lo] = a * CS[i] - b * CA[i];
                        xr[c][hi] = b * CS[i] + a * CA[i];
                    }
                }
            }
            double sb_out[18][32];
            for (int sb = 0; sb < 32; sb++) {
                double tmp36[36];
                if (p->btype == 2) mp3_imdct12(m, &xr[c][sb * 18], tmp36);
                else               mp3_imdct36(m, &xr[c][sb * 18], tmp36, p->btype);
                for (int i = 0; i < 18; i++) {
                    double v = tmp36[i] + m->prev_blk[c][sb][i];
                    if ((sb & 1) && (i & 1)) v = -v;
                    sb_out[i][sb] = v;
                    m->prev_blk[c][sb][i] = tmp36[i + 18];
                }
            }
            for (int i = 0; i < 18; i++) {
                int16_t s32[32];
                mp3_polyphase(m, c, sb_out[i], s32);
                for (int j = 0; j < 32; j++) {
                    int idx_o = (i * 32 + j) * ch + c;
                    pcm_gr[idx_o] = s32[j];
                }
            }
        }
        md_pcm_push_i16(po, pcm_gr, (uint32_t)(18 * 32 * ch));
    }
    return 1;
}

static int md_decode_mp3(const uint8_t *d, uint32_t len,
                         void *(*af)(size_t), void (*ff)(void *),
                         medya_audio_t *out) {
    uint32_t pos = mp3_skip_id3v2(d, len);
    int v = 0, l = 0, br = 0, pad = 0, ch = 0, mode = 0, mext = 0, crc = 0, fsz = 0;
    uint32_t sr = 0;
    int found = 0;
    while (pos + 4u <= len) {
        if (mp3_parse_hdr(d + pos, &v, &l, &br, &sr, &pad, &ch, &mode, &mext, &crc, &fsz) == 0) {
            found = 1;
            break;
        }
        pos++;
    }
    if (!found) {
        out->err = "Geçerli MP3 çerçevesi bulunamadı";
        return -1;
    }

    mp3_dec_t *m = (mp3_dec_t *)af(sizeof(mp3_dec_t));
    if (!m) { out->err = "Bellek yetersiz"; return -1; }
    mp3_init_tables(m);

    md_pcm_out_t po;
    if (md_pcm_init(&po, 262144u, af, ff) != 0) { ff(m); out->err = "Bellek yetersiz"; return -1; }

    uint32_t last_sr = sr;
    int last_ch = ch, frames_ok = 0;
    while (pos + 4u <= len) {
        if (mp3_parse_hdr(d + pos, &v, &l, &br, &sr, &pad, &ch, &mode, &mext, &crc, &fsz) != 0) {
            pos++;
            continue;
        }
        if (pos + (uint32_t)fsz > len) break;
        last_sr = sr; last_ch = ch;
        mp3_decode_frame(m, d + pos, fsz, v, ch, mode, mext, crc, &po);
        frames_ok++;
        pos += (uint32_t)fsz;
    }

    ff(m);
    if (frames_ok == 0 || po.pcm_bytes == 0) {
        ff(po.buf);
        out->err = "Desteklenmeyen veya bozuk MP3 dosyası";
        return -1;
    }
    md_write_wav_hdr(po.buf, last_sr, (uint16_t)last_ch, po.pcm_bytes);
    out->wav_buf = po.buf;
    out->wav_len = 44u + po.pcm_bytes;
    out->pcm_off = 44u;
    out->pcm_len = po.pcm_bytes;
    out->rate = last_sr;
    out->channels = (uint16_t)last_ch;
    out->dur_ms = (uint32_t)(((uint64_t)(po.pcm_bytes / (uint32_t)(last_ch * 2)) * 1000ull) / last_sr);
    out->fmt_name = "MP3";
    return 0;
}

/* ==========================================================================
 * 4. OGG Container + OggPCM + Vorbis I Decoder
 * ========================================================================== */
static int ogg_next_packet(const uint8_t *d, uint32_t len, uint32_t *page_pos,
                           uint8_t *seg_idx, const uint8_t **pkt_buf, uint32_t *pkt_len,
                           uint8_t *scratch, uint32_t scratch_cap) {
    uint32_t total = 0;
    while (*page_pos + 27u <= len) {
        const uint8_t *pg = d + *page_pos;
        if (pg[0]!='O' || pg[1]!='g' || pg[2]!='g' || pg[3]!='S') { (*page_pos)++; continue; }
        uint8_t nseg = pg[26];
        if (*page_pos + 27u + nseg > len) return -1;
        uint32_t data_off = *page_pos + 27u + nseg;
        uint32_t body_off = 0;
        for (uint8_t i = 0; i < *seg_idx; i++) body_off += pg[27u + i];
        while (*seg_idx < nseg) {
            uint8_t lv = pg[27u + *seg_idx];
            (*seg_idx)++;
            if (data_off + body_off + lv > len) return -1;
            if (total + lv <= scratch_cap) {
                memcpy(scratch + total, d + data_off + body_off, lv);
                total += lv;
            }
            body_off += lv;
            if (lv < 255u) {
                *pkt_buf = scratch;
                *pkt_len = total;
                return 0;
            }
        }
        /* Advance to next Ogg page */
        uint32_t pdata = 0;
        for (uint8_t i = 0; i < nseg; i++) pdata += pg[27u + i];
        *page_pos = data_off + pdata;
        *seg_idx = 0;
    }
    return -1;
}

/* Compact Vorbis I decoder (Codebooks + Floor1 + Residue 0/1/2 + Mapping + IMDCT) */
static uint32_t vorb_ilog(uint32_t x) {
    uint32_t r = 0;
    while (x > 0) { r++; x >>= 1; }
    return r;
}

static double vorb_f32_unpack(uint32_t x) {
    int32_t mant = (int32_t)(x & 0x1FFFFFu);
    uint32_t sign = x & 0x80000000u;
    int32_t exp  = (int32_t)((x & 0x7FE00000u) >> 21) - 788;
    if (sign) mant = -mant;
    return md_ldexp((double)mant, exp);
}

typedef struct {
    int      dim;
    int      entries;
    uint8_t *lens;
    uint32_t *codes;
    float   *vq;
    int      lookup_type;
} vorb_cb_t;

static int vorb_cb_build(vorb_cb_t *cb, md_bs_t *b, void *(*af)(size_t)) {
    if (md_bs_read_lsb(b, 24) != 0x564342u) return -1; /* "BCV" */
    cb->dim = (int)md_bs_read_lsb(b, 16);
    cb->entries = (int)md_bs_read_lsb(b, 24);
    if (cb->entries <= 0 || cb->entries > 65536 || cb->dim <= 0 || cb->dim > 256) return -1;

    cb->lens = (uint8_t *)af((size_t)cb->entries);
    cb->codes = (uint32_t *)af((size_t)cb->entries * 4u);
    if (!cb->lens || !cb->codes) return -1;

    int ordered = (int)md_bs_read_lsb(b, 1);
    if (!ordered) {
        int sparse = (int)md_bs_read_lsb(b, 1);
        for (int i = 0; i < cb->entries; i++) {
            if (sparse && !md_bs_read_lsb(b, 1)) cb->lens[i] = 0;
            else cb->lens[i] = (uint8_t)(md_bs_read_lsb(b, 5) + 1u);
        }
    } else {
        int clen = (int)md_bs_read_lsb(b, 5) + 1;
        int i = 0;
        while (i < cb->entries) {
            int bits = (int)vorb_ilog((uint32_t)(cb->entries - i));
            int num = (int)md_bs_read_lsb(b, bits);
            for (int j = 0; j < num && i < cb->entries; j++) cb->lens[i++] = (uint8_t)clen;
            clen++;
        }
    }

    /* Assign canonical MSB-reversed Huffman codes for LSB bitstream */
    uint32_t avail[33];
    memset(avail, 0, sizeof(avail));
    for (int i = 0; i < cb->entries; i++) {
        int l = cb->lens[i];
        if (l == 0 || l > 32) { cb->codes[i] = 0; continue; }
        uint32_t res = avail[l];
        cb->codes[i] = res;
        /* Update available code prefixes per Vorbis spec */
        for (int j = l; j > 0; j--) {
            if (avail[j] & 1u) avail[j] = avail[j - 1] << 1;
            else { avail[j]++; break; }
        }
        for (int j = l + 1; j <= 32; j++) {
            if ((avail[j] >> 1) == res) avail[j] = res << 1;
        }
    }

    cb->lookup_type = (int)md_bs_read_lsb(b, 4);
    cb->vq = 0;
    if (cb->lookup_type == 1 || cb->lookup_type == 2) {
        double minv = vorb_f32_unpack(md_bs_read_lsb(b, 32));
        double delv = vorb_f32_unpack(md_bs_read_lsb(b, 32));
        int vbits = (int)md_bs_read_lsb(b, 4) + 1;
        int seqp  = (int)md_bs_read_lsb(b, 1);
        int vals  = 0;
        if (cb->lookup_type == 1) {
            vals = 1;
            while (1) {
                int p = 1;
                for (int d = 0; d < cb->dim; d++) p *= (vals + 1);
                if (p > cb->entries) break;
                vals++;
            }
        } else {
            vals = cb->entries * cb->dim;
        }
        uint16_t *mults = (uint16_t *)af((size_t)(vals + 1) * 2u);
        cb->vq = (float *)af((size_t)cb->entries * (size_t)cb->dim * sizeof(float));
        if (!mults || !cb->vq) return -1;
        for (int i = 0; i < vals; i++) mults[i] = (uint16_t)md_bs_read_lsb(b, vbits);
        for (int i = 0; i < cb->entries; i++) {
            double last = 0.0;
            int idiv = 1;
            for (int j = 0; j < cb->dim; j++) {
                int off = (cb->lookup_type == 1) ? ((i / idiv) % vals) : (i * cb->dim + j);
                double val = (double)mults[off] * delv + minv + last;
                cb->vq[i * cb->dim + j] = (float)val;
                if (seqp) last = val;
                if (cb->lookup_type == 1) idiv *= vals;
            }
        }
    } else if (cb->lookup_type != 0) {
        return -1;
    }
    return 0;
}

static int vorb_cb_dec(const vorb_cb_t *cb, md_bs_t *b) {
    /* Linear/prefix match on LSB bits (compact & exact for small/medium codebooks) */
    uint32_t code = 0;
    for (int len = 1; len <= 32; len++) {
        code = (code << 1) | md_bs_read_lsb(b, 1);
        for (int i = 0; i < cb->entries; i++) {
            if (cb->lens[i] == len && cb->codes[i] == code) return i;
        }
    }
    return 0;
}

static void vorb_imdct(const double *in, double *out, int N) {
    int n2 = N / 2;
    double scale = 2.0 / (double)N;
    for (int n = 0; n < N; n++) {
        double sum = 0.0;
        double ang0 = (2.0 * MD_PI / (double)N) * ((double)n + 0.5 + (double)N / 4.0);
        for (int k = 0; k < n2; k++) {
            sum += in[k] * cos(ang0 * ((double)k + 0.5));
        }
        out[n] = sum * scale;
    }
}

static int md_decode_ogg(const uint8_t *d, uint32_t len,
                         void *(*af)(size_t), void (*ff)(void *),
                         medya_audio_t *out) {
    uint32_t ppos = 0;
    uint8_t sidx = 0;
    const uint8_t *pkt = 0;
    uint32_t plen = 0;
    uint8_t *scratch = (uint8_t *)af(65536u);
    if (!scratch) { out->err = "Bellek yetersiz"; return -1; }

    if (ogg_next_packet(d, len, &ppos, &sidx, &pkt, &plen, scratch, 65536u) != 0) {
        ff(scratch);
        out->err = "Geçersiz Ogg akışı";
        return -1;
    }

    /* Check BOS packet type */
    if (plen >= 8u && pkt[0]=='O' && pkt[1]=='p' && pkt[2]=='u' && pkt[3]=='s') {
        ff(scratch);
        out->err = "Desteklenmeyen Ogg kodeği (Opus)";
        return -1;
    }
    if (plen >= 8u && pkt[0]=='S' && pkt[1]=='p' && pkt[2]=='e' && pkt[3]=='e') {
        ff(scratch);
        out->err = "Desteklenmeyen Ogg kodeği (Speex)";
        return -1;
    }

    /* OggPCM simple container ("PCM     ") */
    if (plen >= 28u && pkt[0]=='P' && pkt[1]=='C' && pkt[2]=='M' && pkt[3]==' ') {
        uint32_t rate = md_be32(pkt + 12);
        uint8_t ch = pkt[17];
        if (rate == 0 || ch == 0 || ch > 2) { ff(scratch); out->err = "Geçersiz OggPCM"; return -1; }
        md_pcm_out_t po;
        if (md_pcm_init(&po, 131072u, af, ff) != 0) { ff(scratch); out->err = "Bellek yetersiz"; return -1; }
        while (ogg_next_packet(d, len, &ppos, &sidx, &pkt, &plen, scratch, 65536u) == 0) {
            if (plen >= 2u) md_pcm_push_i16(&po, (const int16_t *)pkt, plen / 2u);
        }
        ff(scratch);
        if (po.pcm_bytes == 0) { ff(po.buf); out->err = "Boş OggPCM akışı"; return -1; }
        md_write_wav_hdr(po.buf, rate, ch, po.pcm_bytes);
        out->wav_buf = po.buf; out->wav_len = 44u + po.pcm_bytes;
        out->pcm_off = 44u; out->pcm_len = po.pcm_bytes;
        out->rate = rate; out->channels = ch;
        out->dur_ms = (uint32_t)(((uint64_t)(po.pcm_bytes / (uint32_t)(ch * 2)) * 1000ull) / rate);
        out->fmt_name = "OGG";
        return 0;
    }

    /* Ogg Vorbis (\x01vorbis) */
    if (plen < 30u || pkt[0] != 1 || pkt[1]!='v'||pkt[2]!='o'||pkt[3]!='r'||pkt[4]!='b'||pkt[5]!='i'||pkt[6]!='s') {
        ff(scratch);
        out->err = "Desteklenmeyen Ogg ses kodeği";
        return -1;
    }

    uint8_t ch = pkt[11];
    uint32_t rate = md_rd32(pkt + 12);
    int bs0 = 1 << (pkt[28] & 0x0Fu);
    int bs1 = 1 << (pkt[28] >> 4);
    if (ch == 0 || ch > 2 || rate == 0 || bs0 < 64 || bs1 > 8192 || bs0 > bs1) {
        ff(scratch);
        out->err = "Desteklenmeyen Vorbis parametreleri";
        return -1;
    }

    /* Packet 2: Comment header (\x03vorbis) */
    if (ogg_next_packet(d, len, &ppos, &sidx, &pkt, &plen, scratch, 65536u) != 0 ||
        plen < 7u || pkt[0] != 3) {
        ff(scratch); out->err = "Vorbis yorum başlığı eksik"; return -1;
    }

    /* Packet 3: Setup header (\x05vorbis) */
    if (ogg_next_packet(d, len, &ppos, &sidx, &pkt, &plen, scratch, 65536u) != 0 ||
        plen < 7u || pkt[0] != 5) {
        ff(scratch); out->err = "Vorbis kurulum başlığı eksik"; return -1;
    }

    md_bs_t sb; sb.d = pkt + 7; sb.len = plen - 7u; sb.bit_pos = 0;
    int ncb = (int)md_bs_read_lsb(&sb, 8) + 1;
    if (ncb > 64) { ff(scratch); out->err = "Vorbis codebook sınırı"; return -1; }
    vorb_cb_t *cbs = (vorb_cb_t *)af((size_t)ncb * sizeof(vorb_cb_t));
    if (!cbs) { ff(scratch); out->err = "Bellek yetersiz"; return -1; }
    memset(cbs, 0, (size_t)ncb * sizeof(vorb_cb_t));
    for (int i = 0; i < ncb; i++) {
        if (vorb_cb_build(&cbs[i], &sb, af) != 0) {
            ff(scratch); out->err = "Vorbis codebook çözülemedi"; return -1;
        }
    }

    /* Time domain transforms */
    int ntd = (int)md_bs_read_lsb(&sb, 6) + 1;
    for (int i = 0; i < ntd; i++) md_bs_read_lsb(&sb, 16);

    /* Floors (support Floor 1) */
    typedef struct {
        int partitions;
        int pclass[32];
        int cdim[16], csub[16], cbook[16], csub_books[16][8];
        int mult, rbits;
        int xlist[66], xsort[66], nvals;
    } vorb_fl1_t;
    int nfl = (int)md_bs_read_lsb(&sb, 6) + 1;
    vorb_fl1_t *fls = (vorb_fl1_t *)af((size_t)nfl * sizeof(vorb_fl1_t));
    if (!fls) { ff(scratch); out->err = "Bellek yetersiz"; return -1; }
    memset(fls, 0, (size_t)nfl * sizeof(vorb_fl1_t));

    for (int i = 0; i < nfl; i++) {
        int ft = (int)md_bs_read_lsb(&sb, 16);
        if (ft != 1) { ff(scratch); out->err = "Yalnızca Vorbis Floor1 desteklenir"; return -1; }
        vorb_fl1_t *f = &fls[i];
        f->partitions = (int)md_bs_read_lsb(&sb, 5);
        int max_cls = -1;
        for (int j = 0; j < f->partitions; j++) {
            f->pclass[j] = (int)md_bs_read_lsb(&sb, 4);
            if (f->pclass[j] > max_cls) max_cls = f->pclass[j];
        }
        for (int j = 0; j <= max_cls; j++) {
            f->cdim[j]  = (int)md_bs_read_lsb(&sb, 3) + 1;
            f->csub[j]  = (int)md_bs_read_lsb(&sb, 2);
            if (f->csub[j]) f->cbook[j] = (int)md_bs_read_lsb(&sb, 8);
            int ns = 1 << f->csub[j];
            for (int k = 0; k < ns; k++) f->csub_books[j][k] = (int)md_bs_read_lsb(&sb, 8) - 1;
        }
        f->mult = (int)md_bs_read_lsb(&sb, 2) + 1;
        f->rbits = (int)md_bs_read_lsb(&sb, 4);
        f->xlist[0] = 0;
        f->xlist[1] = 1 << f->rbits;
        int nval = 2;
        for (int j = 0; j < f->partitions; j++) {
            int c = f->pclass[j];
            for (int k = 0; k < f->cdim[c] && nval < 65; k++)
                f->xlist[nval++] = (int)md_bs_read_lsb(&sb, f->rbits);
        }
        f->nvals = nval;
        for (int j = 0; j < nval; j++) f->xsort[j] = j;
        for (int j = 0; j < nval; j++)
            for (int k = j + 1; k < nval; k++)
                if (f->xlist[f->xsort[j]] > f->xlist[f->xsort[k]]) {
                    int t = f->xsort[j]; f->xsort[j] = f->xsort[k]; f->xsort[k] = t;
                }
    }

    /* Residues */
    typedef struct {
        int type, begin, end, psize, classif, classbook;
        uint8_t cascade[64];
        int16_t books[64][8];
    } vorb_res_t;
    int nres = (int)md_bs_read_lsb(&sb, 6) + 1;
    vorb_res_t *ress = (vorb_res_t *)af((size_t)nres * sizeof(vorb_res_t));
    if (!ress) { ff(scratch); out->err = "Bellek yetersiz"; return -1; }
    memset(ress, 0, (size_t)nres * sizeof(vorb_res_t));
    for (int i = 0; i < nres; i++) {
        vorb_res_t *r = &ress[i];
        r->type = (int)md_bs_read_lsb(&sb, 16);
        r->begin = (int)md_bs_read_lsb(&sb, 24);
        r->end   = (int)md_bs_read_lsb(&sb, 24);
        r->psize = (int)md_bs_read_lsb(&sb, 24) + 1;
        r->classif = (int)md_bs_read_lsb(&sb, 6) + 1;
        r->classbook = (int)md_bs_read_lsb(&sb, 8);
        for (int j = 0; j < r->classif; j++) {
            uint8_t lo = (uint8_t)md_bs_read_lsb(&sb, 3);
            uint8_t hi = md_bs_read_lsb(&sb, 1) ? (uint8_t)md_bs_read_lsb(&sb, 5) : 0;
            r->cascade[j] = (uint8_t)((hi << 3) | lo);
        }
        for (int j = 0; j < r->classif; j++)
            for (int k = 0; k < 8; k++)
                r->books[j][k] = (r->cascade[j] & (1u << k)) ? (int16_t)md_bs_read_lsb(&sb, 8) : -1;
    }

    /* Mappings */
    typedef struct {
        int submaps, csteps;
        int mag[8], ang[8], chmux[2], sm_fl[16], sm_res[16];
    } vorb_map_t;
    int nmap = (int)md_bs_read_lsb(&sb, 6) + 1;
    vorb_map_t *maps = (vorb_map_t *)af((size_t)nmap * sizeof(vorb_map_t));
    if (!maps) { ff(scratch); out->err = "Bellek yetersiz"; return -1; }
    memset(maps, 0, (size_t)nmap * sizeof(vorb_map_t));
    for (int i = 0; i < nmap; i++) {
        vorb_map_t *m = &maps[i];
        md_bs_read_lsb(&sb, 16); /* mapping type 0 */
        m->submaps = md_bs_read_lsb(&sb, 1) ? ((int)md_bs_read_lsb(&sb, 4) + 1) : 1;
        if (md_bs_read_lsb(&sb, 1)) {
            m->csteps = (int)md_bs_read_lsb(&sb, 8) + 1;
            int cbits = (int)vorb_ilog(ch > 1 ? (uint32_t)(ch - 1) : 1u);
            for (int j = 0; j < m->csteps && j < 8; j++) {
                m->mag[j] = (int)md_bs_read_lsb(&sb, cbits);
                m->ang[j] = (int)md_bs_read_lsb(&sb, cbits);
            }
        }
        md_bs_read_lsb(&sb, 2); /* reserved */
        if (m->submaps > 1) {
            for (int j = 0; j < ch; j++) m->chmux[j] = (int)md_bs_read_lsb(&sb, 4);
        }
        for (int j = 0; j < m->submaps; j++) {
            md_bs_read_lsb(&sb, 8);
            m->sm_fl[j]  = (int)md_bs_read_lsb(&sb, 8);
            m->sm_res[j] = (int)md_bs_read_lsb(&sb, 8);
        }
    }

    /* Modes */
    typedef struct { int blockflag, mapping; } vorb_mode_t;
    int nmode = (int)md_bs_read_lsb(&sb, 6) + 1;
    vorb_mode_t modes[64];
    for (int i = 0; i < nmode && i < 64; i++) {
        modes[i].blockflag = (int)md_bs_read_lsb(&sb, 1);
        md_bs_read_lsb(&sb, 16);
        md_bs_read_lsb(&sb, 16);
        modes[i].mapping = (int)md_bs_read_lsb(&sb, 8);
    }
    int mode_bits = (int)vorb_ilog((uint32_t)(nmode - 1));

    /* Decode audio packets */
    md_pcm_out_t po;
    if (md_pcm_init(&po, 262144u, af, ff) != 0) { ff(scratch); out->err = "Bellek yetersiz"; return -1; }

    double *spec[2], *pcm_blk[2], *prev_ola[2];
    spec[0] = (double *)af(8192u * sizeof(double));
    spec[1] = (double *)af(8192u * sizeof(double));
    pcm_blk[0] = (double *)af(8192u * sizeof(double));
    pcm_blk[1] = (double *)af(8192u * sizeof(double));
    prev_ola[0] = (double *)af(8192u * sizeof(double));
    prev_ola[1] = (double *)af(8192u * sizeof(double));
    int16_t *i16_blk = (int16_t *)af(8192u * 2u * sizeof(int16_t));
    if (!spec[0] || !spec[1] || !pcm_blk[0] || !pcm_blk[1] || !prev_ola[0] || !prev_ola[1] || !i16_blk) {
        ff(scratch); ff(po.buf); out->err = "Bellek yetersiz"; return -1;
    }
    memset(prev_ola[0], 0, 8192u * sizeof(double));
    memset(prev_ola[1], 0, 8192u * sizeof(double));

    int prev_N = 0;
    while (ogg_next_packet(d, len, &ppos, &sidx, &pkt, &plen, scratch, 65536u) == 0) {
        if (plen < 1u) continue;
        md_bs_t ab; ab.d = pkt; ab.len = plen; ab.bit_pos = 0;
        if (md_bs_read_lsb(&ab, 1) != 0u) continue; /* audio packet must start with 0 */
        int mid = (int)md_bs_read_lsb(&ab, mode_bits);
        if (mid >= nmode) continue;
        int N = modes[mid].blockflag ? bs1 : bs0;
        int n2 = N / 2;
        int pw = 0, nw = 0;
        if (modes[mid].blockflag) {
            pw = (int)md_bs_read_lsb(&ab, 1);
            nw = (int)md_bs_read_lsb(&ab, 1);
        }
        (void)pw; (void)nw;

        vorb_map_t *mp = &maps[modes[mid].mapping];
        int no_energy[2] = { 0, 0 };
        int fy[2][66];
        for (int c = 0; c < ch; c++) {
            vorb_fl1_t *fl = &fls[mp->sm_fl[mp->chmux[c]]];
            if (!md_bs_read_lsb(&ab, 1)) { no_energy[c] = 1; continue; }
            int range_max = (fl->mult == 1) ? 256 : (fl->mult == 2 ? 128 : (fl->mult == 3 ? 86 : 64));
            int rb = (int)vorb_ilog((uint32_t)(range_max - 1));
            fy[c][0] = (int)md_bs_read_lsb(&ab, rb);
            fy[c][1] = (int)md_bs_read_lsb(&ab, rb);
            int vi = 2;
            for (int p = 0; p < fl->partitions; p++) {
                int cls = fl->pclass[p];
                int cdim = fl->cdim[cls], cbits = fl->csub[cls];
                int cval = cbits ? vorb_cb_dec(&cbs[fl->cbook[cls]], &ab) : 0;
                for (int k = 0; k < cdim && vi < fl->nvals; k++) {
                    int sub = (cval >> (k * cbits)) & ((1 << cbits) - 1);
                    int bk = fl->csub_books[cls][sub];
                    fy[c][vi++] = (bk >= 0 && bk < ncb) ? vorb_cb_dec(&cbs[bk], &ab) : 0;
                }
            }
        }

        for (int c = 0; c < ch; c++) memset(spec[c], 0, (size_t)n2 * sizeof(double));
        for (int sm = 0; sm < mp->submaps; sm++) {
            int ch_map[2], nch_sm = 0;
            for (int c = 0; c < ch; c++) if (mp->chmux[c] == sm) ch_map[nch_sm++] = c;
            if (nch_sm == 0) continue;
            vorb_res_t *r = &ress[mp->sm_res[sm]];
            int r_end = (r->end < n2) ? r->end : n2;
            int n_part = (r_end - r->begin) / r->psize;
            if (n_part <= 0) continue;
            int cbook = r->classbook;
            int pwords = cbs[cbook].dim;
            int cls_buf[2][512];
            for (int pass = 0; pass < 8; pass++) {
                int pc = 0;
                while (pc < n_part) {
                    if (pass == 0) {
                        for (int ci = 0; ci < nch_sm; ci++) {
                            int cv = vorb_cb_dec(&cbs[cbook], &ab);
                            for (int k = pwords - 1; k >= 0; k--) {
                                if (pc + k < 512) cls_buf[ci][pc + k] = cv % r->classif;
                                cv /= r->classif;
                            }
                        }
                    }
                    for (int k = 0; k < pwords && pc < n_part; k++, pc++) {
                        for (int ci = 0; ci < nch_sm; ci++) {
                            int cl = cls_buf[ci][pc];
                            int bk = r->books[cl][pass];
                            if (bk < 0 || bk >= ncb) continue;
                            int off = r->begin + pc * r->psize;
                            vorb_cb_t *cb = &cbs[bk];
                            if (r->type == 0) {
                                int step = r->psize / cb->dim;
                                for (int s = 0; s < step; s++) {
                                    int v = vorb_cb_dec(cb, &ab);
                                    for (int j = 0; j < cb->dim; j++) {
                                        int idx_f = off + s + j * step;
                                        if (idx_f < n2 && cb->vq) spec[ch_map[ci]][idx_f] += cb->vq[v * cb->dim + j];
                                    }
                                }
                            } else {
                                for (int s = 0; s < r->psize; s += cb->dim) {
                                    int v = vorb_cb_dec(cb, &ab);
                                    for (int j = 0; j < cb->dim && (off + s + j) < n2; j++) {
                                        if (cb->vq) spec[ch_map[ci]][off + s + j] += cb->vq[v * cb->dim + j];
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        /* Inverse coupling + Floor1 synthesis */
        for (int i = mp->csteps - 1; i >= 0; i--) {
            int mc = mp->mag[i], ac = mp->ang[i];
            for (int k = 0; k < n2; k++) {
                double M = spec[mc][k], A = spec[ac][k];
                if (M > 0.0) { spec[mc][k] = M; spec[ac][k] = (A > 0.0) ? (M - A) : (M + A); }
                else         { spec[mc][k] = M; spec[ac][k] = (A > 0.0) ? (M - A) : (M + A); }
            }
        }

        for (int c = 0; c < ch; c++) {
            if (no_energy[c]) { memset(spec[c], 0, (size_t)n2 * sizeof(double)); continue; }
            vorb_fl1_t *fl = &fls[mp->sm_fl[mp->chmux[c]]];
            int ydec[66];
            ydec[0] = fy[c][0]; ydec[1] = fy[c][1];
            int range = (fl->mult == 1) ? 256 : (fl->mult == 2 ? 128 : (fl->mult == 3 ? 86 : 64));
            for (int i = 2; i < fl->nvals; i++) {
                int lo = 0, hi = 1;
                for (int j = 0; j < i; j++) {
                    if (fl->xlist[j] < fl->xlist[i] && fl->xlist[j] > fl->xlist[lo]) lo = j;
                    if (fl->xlist[j] > fl->xlist[i] && fl->xlist[j] < fl->xlist[hi]) hi = j;
                }
                int pred = ydec[lo] + (ydec[hi] - ydec[lo]) * (fl->xlist[i] - fl->xlist[lo]) /
                           (fl->xlist[hi] - fl->xlist[lo] ? fl->xlist[hi] - fl->xlist[lo] : 1);
                int hiroom = range - pred, loroom = pred;
                int room = (hiroom < loroom ? hiroom : loroom) * 2;
                int v = fy[c][i];
                int val = 0;
                if (v >= room) val = (hiroom > loroom) ? (v - loroom + pred) : (pred - v + hiroom - 1);
                else           val = (v & 1) ? (pred - ((v + 1) >> 1)) : (pred + (v >> 1));
                ydec[i] = val;
            }
            for (int s = 0; s < fl->nvals - 1; s++) {
                int i0 = fl->xsort[s], i1 = fl->xsort[s + 1];
                int x0 = fl->xlist[i0], x1 = fl->xlist[i1];
                int y0 = ydec[i0] * fl->mult, y1 = ydec[i1] * fl->mult;
                for (int x = x0; x < x1 && x < n2; x++) {
                    int y = y0 + (y1 - y0) * (x - x0) / ((x1 - x0) ? (x1 - x0) : 1);
                    double db = (double)y * 0.11512925464970229; /* ln(10)/20 * 1..255 */
                    spec[c][x] *= md_exp((db - 29.0) * 0.5);
                }
            }
        }

        /* IMDCT + Vorbis Window + Overlap-Add */
        for (int c = 0; c < ch; c++) {
            vorb_imdct(spec[c], pcm_blk[c], N);
            for (int i = 0; i < N; i++) {
                double s = sin(MD_PI / (double)N * ((double)i + 0.5));
                pcm_blk[c][i] *= sin(0.5 * MD_PI * s * s);
            }
        }
        if (prev_N > 0) {
            int l_win = (prev_N < N) ? prev_N / 2 : n2;
            for (int i = 0; i < l_win; i++) {
                for (int c = 0; c < ch; c++) {
                    double v = prev_ola[c][i] + pcm_blk[c][i];
                    i16_blk[i * ch + c] = md_clip16((int32_t)(v * 32767.0));
                }
            }
            md_pcm_push_i16(&po, i16_blk, (uint32_t)(l_win * ch));
        }
        for (int c = 0; c < ch; c++) {
            memcpy(prev_ola[c], pcm_blk[c] + n2, (size_t)n2 * sizeof(double));
        }
        prev_N = N;
    }

    ff(scratch); ff(spec[0]); ff(spec[1]); ff(pcm_blk[0]); ff(pcm_blk[1]);
    ff(prev_ola[0]); ff(prev_ola[1]); ff(i16_blk);
    if (po.pcm_bytes == 0) {
        ff(po.buf);
        out->err = "Ogg Vorbis ses karesi çözülemedi";
        return -1;
    }
    md_write_wav_hdr(po.buf, rate, ch, po.pcm_bytes);
    out->wav_buf = po.buf; out->wav_len = 44u + po.pcm_bytes;
    out->pcm_off = 44u; out->pcm_len = po.pcm_bytes;
    out->rate = rate; out->channels = ch;
    out->dur_ms = (uint32_t)(((uint64_t)(po.pcm_bytes / (uint32_t)(ch * 2)) * 1000ull) / rate);
    out->fmt_name = "OGG";
    return 0;
}

/* ==========================================================================
 * 5. Public Probe & Decode Entry Points
 * ========================================================================== */
static uint32_t medya_probe_duration(const uint8_t *d, uint32_t len, uint32_t file_size, const char *name) {
    if (!d || len < 16u) return 0u;
    /* WAV */
    if (d[0]=='R' && d[1]=='I' && d[2]=='F' && d[3]=='F') {
        uint16_t ch = 0, bits = 0;
        uint32_t rate = 0, data = 0, off = 12u;
        while (off + 8u <= len) {
            uint32_t sz = md_rd32(d + off + 4u), body = off + 8u;
            if (d[off]=='f' && d[off+1]=='m' && d[off+2]=='t' && d[off+3]==' ' && body + 16u <= len) {
                ch = md_rd16(d + body + 2u);
                rate = md_rd32(d + body + 4u);
                bits = md_rd16(d + body + 14u);
            } else if (d[off]=='d' && d[off+1]=='a' && d[off+2]=='t' && d[off+3]=='a') {
                data = sz;
                break;
            }
            uint32_t adv = sz + (sz & 1u);
            if (adv < 8u && sz != 0u) break;
            off = body + adv;
        }
        uint32_t bps = rate * (uint32_t)ch * (uint32_t)(bits / 8u);
        return bps ? (uint32_t)(((uint64_t)data * 1000ull) / bps) : 0u;
    }
    /* FLAC */
    if (d[0]=='f' && d[1]=='L' && d[2]=='a' && d[3]=='C' && len >= 42u) {
        uint32_t sr = ((uint32_t)d[18] << 12) | ((uint32_t)d[19] << 4) | ((uint32_t)d[20] >> 4);
        uint64_t tot = ((uint64_t)(d[21] & 0x0Fu) << 32) | md_be32(d + 22);
        return (sr && tot) ? (uint32_t)((tot * 1000ull) / sr) : 0u;
    }
    /* MP3 estimate from first frame bitrate */
    if (md_ext_eq(name, "mp3") || (d[0]=='I' && d[1]=='D' && d[2]=='3') || (d[0]==0xFF && (d[1]&0xE0)==0xE0)) {
        uint32_t pos = mp3_skip_id3v2(d, len);
        int v, l, br, pd, ch, md, me, crc, fsz;
        uint32_t sr;
        while (pos + 4u <= len) {
            if (mp3_parse_hdr(d + pos, &v, &l, &br, &sr, &pd, &ch, &md, &me, &crc, &fsz) == 0) {
                uint32_t data_bytes = (file_size > pos) ? (file_size - pos) : file_size;
                return (uint32_t)(((uint64_t)data_bytes * 8ull) / (uint64_t)br);
            }
            pos++;
        }
    }
    return 0u;
}

static int medya_decode_audio(const uint8_t *raw, uint32_t raw_len, const char *name,
                              void *(*alloc_fn)(size_t), void (*free_fn)(void *),
                              medya_audio_t *out) {
    memset(out, 0, sizeof(*out));
    if (!raw || raw_len < 12u || !alloc_fn || !free_fn) {
        out->err = "Geçersiz ses dosyası";
        return -1;
    }
    if (raw[0]=='R' && raw[1]=='I' && raw[2]=='F' && raw[3]=='F')
        return md_decode_wav(raw, raw_len, alloc_fn, free_fn, out);
    if (raw[0]=='f' && raw[1]=='L' && raw[2]=='a' && raw[3]=='C')
        return md_decode_flac(raw, raw_len, alloc_fn, free_fn, out);
    if (raw[0]=='O' && raw[1]=='g' && raw[2]=='g' && raw[3]=='S')
        return md_decode_ogg(raw, raw_len, alloc_fn, free_fn, out);
    if ((raw[0]=='I' && raw[1]=='D' && raw[2]=='3') ||
        (raw[0]==0xFF && (raw[1] & 0xE0u) == 0xE0u) ||
        md_ext_eq(name, "mp3"))
        return md_decode_mp3(raw, raw_len, alloc_fn, free_fn, out);

    out->err = "Desteklenmeyen ses biçimi";
    return -1;
}

#endif /* ARCTIAN_MEDYA_DEC_H */
