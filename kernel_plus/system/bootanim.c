/**
 * Acilis animasyonu (kernel_plus)
 *
 * Referans sahne: Arctian OS acilis animasyonu (1280x720).
 *   - 288 cokgenli arka plan (renk + opaklik + parildama)
 *   - 200x200 logo (yumusak buyume)
 *   - "ARCTIAN OS" basligi
 *   - 360x6 ilerleme cubugu
 *
 * Cubuk gercek acilis ilerlemesini gosterir: bootanim_advance() cagrilari
 * kernel_plus_main icindeki adimlardan gelir. Sahte zamanlayici yoktur.
 *
 * Cizim, VESA arkatamponu uzerine yapilir; animasyon kendi cekirdek
 * is parciginda calisir, boylece surucu baslatma surerken de akar.
 */
#include "bootanim.h"
#include "backbuffer.h"
#include "clock.h"
#include "sched.h"
#include "serial.h"
#include "memory_map.h"
#include <stdint.h>

#define REF_W 1280
#define REF_H 720

#define PAGE_BG   0x05080CU
#define ACCENT    0x38B6EEU
#define BAR_BG    0x162636U

#define LOGO_W 200
#define LOGO_H 200
#define LOGO_X 540
#define LOGO_Y 244

#define TITLE_W 206
#define TITLE_H 15
#define TITLE_X 537
#define TITLE_Y 459

#define BAR_X 460
#define BAR_Y 640
#define BAR_W 360
#define BAR_H 6

#define FACET_MS   1100U   /* facet animasyon suresi */
#define FACET_HOLD 14418U  /* 0.22 * 65535: renk gecisi buradan sonra baslar */
#define FADE_MS    770U    /* sahne kararma suresi (11s * 0.07) */

#define TRI_MAXW 256

extern const uint8_t banim_logo_argb[];
extern const uint8_t banim_title_argb[];

static int        s_w, s_h;
static volatile int s_started;
static volatile int s_stop;
static volatile int s_stopped;
static int        s_threaded;
static sched_tid_t s_tid = -1;

static uint64_t   s_t0;
static volatile uint32_t s_target;  /* gercek ilerleme, binde   */
static volatile uint32_t s_disp;    /* cubuga yansiyan deger    */
static volatile uint64_t s_fade_at; /* 0 = kararma yok          */

static int16_t s_tx[BANIM_MAX_FACETS][3];
static int16_t s_ty[BANIM_MAX_FACETS][3];

static uint32_t s_cov[TRI_MAXW];

/* ---------------------------------------------------------------- yardimci */

#define DIV255(x) (((x) + 1u + ((x) >> 8)) >> 8)

static uint32_t mix_rgb(uint32_t d, uint32_t s, uint32_t a) {
    uint32_t ia = 255u - a;
    uint32_t r = DIV255((((s >> 16) & 255u) * a) + (((d >> 16) & 255u) * ia));
    uint32_t g = DIV255((((s >> 8) & 255u) * a) + (((d >> 8) & 255u) * ia));
    uint32_t b = DIV255(((s & 255u) * a) + ((d & 255u) * ia));
    return (r << 16) | (g << 8) | b;
}

static uint32_t pack16(const uint16_t c[3]) {
    uint32_t r = (c[0] * 255u + 32768u) >> 16;
    uint32_t g = (c[1] * 255u + 32768u) >> 16;
    uint32_t b = (c[2] * 255u + 32768u) >> 16;
    return (r << 16) | (g << 8) | b;
}

static uint32_t lerp16(const uint16_t a[3], const uint16_t b[3], uint32_t f) {
    uint16_t c[3];
    for (int i = 0; i < 3; i++)
        c[i] = (uint16_t)(((uint32_t)a[i] * (65535u - f) + (uint32_t)b[i] * f) >> 16);
    return pack16(c);
}

/* 0..65535 giris -> egri tablosundan 0..65535 cikis */
static uint32_t ease(const uint16_t *lut, uint32_t t) {
    if (t >= 65535u) return lut[256];
    uint32_t pos = t >> 8;              /* 0..255 */
    uint32_t frac = (t & 255u) << 8;    /* 0..65280 */
    uint32_t a = lut[pos];
    uint32_t b = lut[pos + 1];
    return a + (uint32_t)(((uint64_t)(b - a) * frac) >> 16);
}

static uint32_t scale_x(int v) { return (uint32_t)v * (uint32_t)s_w / REF_W; }
static uint32_t scale_y(int v) { return (uint32_t)v * (uint32_t)s_h / REF_H; }

/* ---------------------------------------------------------------- ucgen */

static void draw_tri(const int16_t *x10, const int16_t *y10, uint32_t rgb, uint32_t alpha) {
    if (alpha == 0) return;

    int xmin = x10[0], xmax = x10[0];
    int ymin = y10[0], ymax = y10[0];
    for (int i = 1; i < 3; i++) {
        if (x10[i] < xmin) xmin = x10[i];
        if (x10[i] > xmax) xmax = x10[i];
        if (y10[i] < ymin) ymin = y10[i];
        if (y10[i] > ymax) ymax = y10[i];
    }
    if (xmax <= xmin || ymax <= ymin) return;

    int px0 = xmin / 10;
    int px1 = (xmax + 9) / 10;
    int py0 = ymin / 10;
    int py1 = (ymax + 9) / 10;
    if (px1 < 0 || py1 < 0 || px0 >= s_w || py0 >= s_h) return;

    int bx0 = px0 < 0 ? 0 : px0;
    int bx1 = px1 >= s_w ? s_w - 1 : px1;
    if (bx0 > bx1) return;
    int n = bx1 - bx0 + 1;
    if (n > TRI_MAXW) { bx1 = bx0 + TRI_MAXW - 1; n = TRI_MAXW; }

    int32_t X[3], Y[3];
    for (int i = 0; i < 3; i++) { X[i] = x10[i] * 10; Y[i] = y10[i] * 10; }

    uint32_t *fb = (uint32_t *)BACKBUFFER_BASE;

    for (int y = py0; y <= py1; y++) {
        if (y < 0 || y >= s_h) continue;

        for (int i = 0; i < n; i++) s_cov[i] = 0;

        for (int sub = 0; sub < 2; sub++) {
            int32_t ys = y * 100 + (sub ? 75 : 25);
            int64_t xl = 0, xr = 0;
            int got = 0;

            for (int e = 0; e < 3; e++) {
                int32_t ya = Y[e], yb = Y[(e + 1) % 3];
                if (!((ya <= ys && yb > ys) || (yb <= ys && ya > ys))) continue;
                int32_t xa = X[e], xb = X[(e + 1) % 3];
                int64_t x = (int64_t)xa + ((int64_t)(xb - xa) * (ys - ya)) / (yb - ya);
                if (!got) { xl = xr = x; got = 1; }
                else { if (x < xl) xl = x; if (x > xr) xr = x; }
            }
            if (!got || xr <= xl) continue;

            int i0 = (int)(xl / 100);
            int i1 = (int)(xr / 100);
            int f0 = (int)(xl % 100);
            int f1 = (int)(xr % 100);
            if (f0 < 0) { f0 += 100; i0--; }
            if (f1 < 0) { f1 += 100; i1--; }
            if (i1 < bx0 || i0 > bx1 || i0 > i1) continue;

            if (i0 < bx0) { i0 = bx0; f0 = 0; }
            if (i1 > bx1) { i1 = bx1; f1 = 100; }

            if (i0 == i1) {
                int w = (int)(xr - xl);
                if (w > 100) w = 100;
                s_cov[i0 - bx0] += (uint32_t)w * 327u;
            } else {
                s_cov[i0 - bx0] += (uint32_t)(100 - f0) * 327u;
                for (int i = i0 + 1; i < i1; i++)
                    s_cov[i - bx0] += 32768u;
                s_cov[i1 - bx0] += (uint32_t)f1 * 327u;
            }
        }

        uint32_t *row = fb + (uint32_t)y * (uint32_t)s_w;
        if (alpha == 255u) {
            for (int i = 0; i < n; i++) {
                uint32_t c = s_cov[i];
                if (c == 0) continue;
                if (c >= 65535u) row[bx0 + i] = rgb;
                else row[bx0 + i] = mix_rgb(row[bx0 + i], rgb, (c * 255u + 32768u) >> 16);
            }
        } else {
            for (int i = 0; i < n; i++) {
                uint32_t c = s_cov[i];
                if (c == 0) continue;
                if (c > 65535u) c = 65535u;
                uint32_t a = (c * alpha + 32768u) >> 16;
                if (a == 0) continue;
                row[bx0 + i] = mix_rgb(row[bx0 + i], rgb, a);
            }
        }
    }
}

/* ---------------------------------------------------------------- gorsel */

static void blit_image(const uint8_t *img, int iw, int ih, int dx, int dy,
                       int dw, int dh, uint32_t opacity) {
    if (opacity == 0 || dw <= 0 || dh <= 0) return;
    uint32_t *fb = (uint32_t *)BACKBUFFER_BASE;
    const uint32_t *src = (const uint32_t *)img;

    if (dw == iw && dh == ih) {
        for (int y = 0; y < dh; y++) {
            int sy = dy + y;
            if (sy < 0 || sy >= s_h) continue;
            for (int x = 0; x < dw; x++) {
                int sx = dx + x;
                if (sx < 0 || sx >= s_w) continue;
                uint32_t p = src[(uint32_t)y * (uint32_t)iw + (uint32_t)x];
                uint32_t a = (((p >> 24) & 255u) * opacity + 32768u) >> 16;
                if (a == 0) continue;
                fb[(uint32_t)sy * (uint32_t)s_w + (uint32_t)sx] =
                    mix_rgb(fb[(uint32_t)sy * (uint32_t)s_w + (uint32_t)sx], p & 0xFFFFFFu, a);
            }
        }
        return;
    }

    for (int y = 0; y < dh; y++) {
        int sy = dy + y;
        if (sy < 0 || sy >= s_h) continue;
        int64_t u = ((int64_t)(y * 2 + 1) * ih * 32768) / dh - 32768;
        int y0 = (int)(u >> 16);
        int fy = (int)((u >> 8) & 255);
        if (y0 < 0) { y0 = 0; fy = 0; }
        if (y0 >= ih - 1) { y0 = ih - 2; fy = 255; }
        if (y0 < 0) y0 = 0;

        for (int x = 0; x < dw; x++) {
            int sx = dx + x;
            if (sx < 0 || sx >= s_w) continue;
            int64_t v = ((int64_t)(x * 2 + 1) * iw * 32768) / dw - 32768;
            int x0 = (int)(v >> 16);
            int fx = (int)((v >> 8) & 255);
            if (x0 < 0) { x0 = 0; fx = 0; }
            if (x0 >= iw - 1) { x0 = iw - 2; fx = 255; }
            if (x0 < 0) x0 = 0;

            const uint32_t *r0 = src + (uint32_t)y0 * (uint32_t)iw + (uint32_t)x0;
            const uint32_t *r1 = r0 + (uint32_t)iw;
            uint32_t p00 = r0[0], p10 = r0[1], p01 = r1[0], p11 = r1[1];

            uint32_t w00 = (uint32_t)(255 - fx) * (uint32_t)(255 - fy);
            uint32_t w10 = (uint32_t)fx * (uint32_t)(255 - fy);
            uint32_t w01 = (uint32_t)(255 - fx) * (uint32_t)fy;
            uint32_t w11 = (uint32_t)fx * (uint32_t)fy;

            uint32_t a = (((p00 >> 24) & 255u) * w00 + ((p10 >> 24) & 255u) * w10 +
                          ((p01 >> 24) & 255u) * w01 + ((p11 >> 24) & 255u) * w11) >> 16;
            if (a == 0) continue;
            a = (a * opacity + 32768u) >> 16;
            if (a == 0) continue;

            uint32_t r = (((p00 >> 16) & 255u) * w00 + ((p10 >> 16) & 255u) * w10 +
                          ((p01 >> 16) & 255u) * w01 + ((p11 >> 16) & 255u) * w11) >> 16;
            uint32_t g = (((p00 >> 8) & 255u) * w00 + ((p10 >> 8) & 255u) * w10 +
                          ((p01 >> 8) & 255u) * w01 + ((p11 >> 8) & 255u) * w11) >> 16;
            uint32_t b = ((p00 & 255u) * w00 + (p10 & 255u) * w10 +
                          (p01 & 255u) * w01 + (p11 & 255u) * w11) >> 16;

            uint32_t *dst = fb + (uint32_t)sy * (uint32_t)s_w + (uint32_t)sx;
            *dst = mix_rgb(*dst, (r << 16) | (g << 8) | b, a);
        }
    }
}

static void draw_bar(uint32_t opacity, uint32_t frac) {
    if (opacity == 0) return;
    uint32_t *fb = (uint32_t *)BACKBUFFER_BASE;
    int bx = (int)scale_x(BAR_X);
    int by = (int)scale_y(BAR_Y);
    int bw = (int)scale_x(BAR_W);
    int bh = (int)scale_y(BAR_H);
    if (bw <= 0 || bh <= 0) return;

    for (int y = 0; y < bh; y++) {
        int sy = by + y;
        if (sy < 0 || sy >= s_h) continue;
        /* clip-path: polygon(6 0, 100% 0, 100%-6 100%, 0 100%) */
        int den = (bh > 1) ? bh - 1 : 1;
        int l = 6 * (bh - 1 - y) / den;
        int r = bw - 6 * y / den;
        int fillw = (int)(((uint64_t)bw * frac) >> 16);
        if (fillw > r) fillw = r;

        for (int x = l; x < r; x++) {
            int sx = bx + x;
            if (sx < 0 || sx >= s_w) continue;
            uint32_t *dst = fb + (uint32_t)sy * (uint32_t)s_w + (uint32_t)sx;
            uint32_t col = (x < fillw) ? ACCENT : BAR_BG;
            *dst = mix_rgb(*dst, col, (uint32_t)((opacity * 255u + 32768u) >> 16));
        }
    }
}

/* ---------------------------------------------------------------- sahne */

static void render_frame(uint64_t t) {
    int total = s_w * s_h;
    uint32_t *fb = (uint32_t *)BACKBUFFER_BASE;

    {
        uint64_t c = ((uint64_t)PAGE_BG << 32) | PAGE_BG;
        uint64_t *p = (uint64_t *)fb;
        int n = total / 2;
        for (int i = 0; i < n; i++) p[i] = c;
        if (total & 1) fb[total - 1] = PAGE_BG;
    }

    /* --- facet arka plan --- */
    for (uint32_t i = 0; i < banim_facet_count; i++) {
        const banim_facet_t *f = &banim_facets[i];
        if (t < f->delay) continue;

        uint32_t e = (uint32_t)(t - f->delay);
        if (e > FACET_MS) e = FACET_MS;
        uint32_t ep = (uint32_t)((uint64_t)e * 65535u / FACET_MS);

        uint32_t color = pack16(f->hi);
        uint32_t opa;
        if (ep < FACET_HOLD) {
            opa = ease(banim_ease_out, (uint32_t)((uint64_t)ep * 65535u / FACET_HOLD));
        } else {
            uint32_t ct = (uint32_t)((uint64_t)(ep - FACET_HOLD) * 65535u / (65535u - FACET_HOLD));
            color = lerp16(f->hi, f->lo, ease(banim_ease_out, ct));
            opa = 65535u;
        }

        if (f->tw_dur) {
            if (t >= f->tw_delay) {
                uint32_t ph = (uint32_t)((t - f->tw_delay) % f->tw_dur);
                ph = (uint32_t)((uint64_t)ph * 65535u / f->tw_dur);
                if (ph < 32768u) {
                    uint32_t k = ease(banim_ease_in_out, ph * 2u);
                    opa = 65535u - (uint32_t)(((uint64_t)(65535u - 36045u) * k) >> 16);
                } else {
                    uint32_t k = ease(banim_ease_in_out, (ph - 32768u) * 2u);
                    opa = 36045u + (uint32_t)(((uint64_t)(65535u - 36045u) * k) >> 16);
                }
            }
        }

        draw_tri(s_tx[i], s_ty[i], color, (opa * 255u + 32768u) >> 16);
    }

    /* --- logo --- */
    if (t >= 1100) {
        uint32_t p = (uint32_t)(t - 1100);
        if (p > 900) p = 900;
        p = ease(banim_ease_logo, (uint32_t)((uint64_t)p * 65535u / 900));
        uint32_t opa = p;
        uint32_t s = 60293u + (uint32_t)(((uint64_t)5243u * p) >> 16); /* 0.92 -> 1.0 */
        int dw = (int)(((uint64_t)scale_x(LOGO_W) * s) >> 16);
        int dh = (int)(((uint64_t)scale_y(LOGO_H) * s) >> 16);
        int dx = (int)scale_x(LOGO_X) + (int)((scale_x(LOGO_W) - dw) / 2);
        int dy = (int)scale_y(LOGO_Y) + (int)((scale_y(LOGO_H) - dh) / 2);
        blit_image(banim_logo_argb, LOGO_W, LOGO_H, dx, dy, dw, dh, opa);
    }

    /* --- baslik --- */
    if (t >= 1600) {
        uint32_t p = (uint32_t)(t - 1600);
        if (p > 800) p = 800;
        p = ease(banim_ease_out, (uint32_t)((uint64_t)p * 65535u / 800));
        blit_image(banim_title_argb, TITLE_W, TITLE_H,
                   (int)scale_x(TITLE_X), (int)scale_y(TITLE_Y),
                   (int)scale_x(TITLE_W), (int)scale_y(TITLE_H), p);
    }

    /* --- ilerleme cubugu --- */
    if (t >= 1200) {
        uint32_t p = (uint32_t)(t - 1200);
        if (p > 500) p = 500;
        p = ease(banim_ease_out, (uint32_t)((uint64_t)p * 65535u / 500));
        uint32_t frac = (uint32_t)((uint64_t)s_disp * 65535u / 1000u);
        draw_bar(p, frac);
    }

    /* --- sahne kararmasi --- */
    uint64_t fade = s_fade_at;
    if (fade) {
        uint64_t now = clock_ms();
        uint32_t k;
        if (now <= fade) k = 0;
        else if (now - fade >= FADE_MS) k = 65535;
        else k = (uint32_t)(((now - fade) * 65535u) / FADE_MS);
        if (k) {
            uint32_t a = (k * 255u + 32768u) >> 16;
            if (a >= 255u) {
                uint64_t c = ((uint64_t)PAGE_BG << 32) | PAGE_BG;
                uint64_t *p64 = (uint64_t *)fb;
                int n = total / 2;
                for (int i = 0; i < n; i++) p64[i] = c;
                if (total & 1) fb[total - 1] = PAGE_BG;
            } else {
                for (int i = 0; i < total; i++) fb[i] = mix_rgb(fb[i], PAGE_BG, (uint32_t)a);
            }
        }
    }
}

static void render_once(void) {
    uint64_t now = clock_ms();
    if (!s_started) return;

    /* gercek ilerlemeyi yumusak takip et */
    uint32_t target = s_target;
    uint32_t disp = s_disp;
    if (disp != target) {
        int32_t d = (int32_t)target - (int32_t)disp;
        if (d > 0) disp += (uint32_t)((d + 5) / 6);
        else disp -= (uint32_t)((-d + 5) / 6);
        if (disp > 1000u) disp = 1000u;
        s_disp = disp;
    }

    render_frame(now - s_t0);
    backbuffer_blit();
}

static void anim_thread(void *arg) {
    (void)arg;
    while (!s_stop) {
        render_once();
        sched_sleep_ms(16);
    }
    s_stopped = 1;
    sched_exit();
}

/* ---------------------------------------------------------------- API */

void bootanim_start(void) {
    if (s_started) return;
    if (!backbuffer_is_active()) return;

    s_w = backbuffer_width();
    s_h = backbuffer_height();
    if (s_w <= 0 || s_h <= 0) return;

    for (uint32_t i = 0; i < banim_facet_count; i++) {
        for (int k = 0; k < 3; k++) {
            s_tx[i][k] = (int16_t)((int32_t)banim_facets[i].x[k] * s_w / REF_W);
            s_ty[i][k] = (int16_t)((int32_t)banim_facets[i].y[k] * s_h / REF_H);
        }
    }

    s_target = 0;
    s_disp = 0;
    s_t0 = clock_ms();
    s_started = 1;
    serial_puts("[banim] start\n");

    s_tid = sched_spawn("bootanim", anim_thread, 0);
    s_threaded = (s_tid >= 0);
    if (!s_threaded) render_once();
}

void bootanim_set(uint32_t permille) {
    if (permille > 1000u) permille = 1000u;
    s_target = permille;
    if (s_started && !s_threaded) render_once();
}

void bootanim_advance(uint32_t permille) {
    uint32_t v = s_target + permille;
    if (v > 1000u) v = 1000u;
    s_target = v;
    if (s_started && !s_threaded) render_once();
}

void bootanim_finish(uint32_t min_ms) {
    if (!s_started) return;

    uint64_t last = 0;
    while (clock_ms() - s_t0 < min_ms || s_target < 1000u) {
        if (!s_threaded) {
            uint64_t now = clock_ms();
            if (now - last >= 30) { last = now; render_once(); }
        }
        sched_sleep_ms(10);
    }

    /* son ilerlemeyi cubuga isle */
    s_disp = 1000u;
    s_fade_at = clock_ms() + 1;
    uint64_t start = clock_ms();
    while (clock_ms() - start < FADE_MS + 40u) {
        if (!s_threaded) {
            uint64_t now = clock_ms();
            if (now - last >= 30) { last = now; render_once(); }
        }
        sched_sleep_ms(10);
    }

    s_stop = 1;
    if (s_threaded) {
        for (int i = 0; i < 400 && !s_stopped; i++) sched_sleep_ms(2);
        if (s_tid >= 0) sched_reap(s_tid);
    }
    s_started = 0;
}
