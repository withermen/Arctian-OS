/*
 * Arctian Web Engine — Freestanding HTML5 <canvas> 2D Software Rasterizer
 * Zero 3rd-party dependencies, zero libc, zero libm.
 * Renders directly to ARGB8888 pixel buffer (0xAARRGGBB).
 */
#ifndef ARCTIAN_BROWSER_CANVAS_H
#define ARCTIAN_BROWSER_CANVAS_H

#include <stdint.h>
#include <stddef.h>

#define BC_MAX_PTS    1024
#define BC_MAX_SUBS   64
#define BC_MAX_STACK  16
#define BC_MAX_STOPS  8

typedef struct {
    double off;
    uint32_t col;
} bc_stop_t;

typedef struct {
    int active;
    double x0, y0, x1, y1;
    bc_stop_t stops[BC_MAX_STOPS];
    int nstops;
} bc_grad_t;

typedef struct {
    uint32_t fill_col;
    uint32_t stroke_col;
    bc_grad_t fill_grad;
    double line_width;
    double alpha;
    int font_px;
    double m[6]; /* 2D affine: x' = m[0]*x + m[2]*y + m[4], y' = m[1]*x + m[3]*y + m[5] */
} bc_state_t;

typedef struct {
    uint32_t *px;
    int w, h;
    bc_state_t st;
    bc_state_t stack[BC_MAX_STACK];
    int sp;
    double px_pts[BC_MAX_PTS];
    double py_pts[BC_MAX_PTS];
    int npts;
    int sub_start[BC_MAX_SUBS];
    int sub_closed[BC_MAX_SUBS];
    int nsubs;
} bc_ctx_t;

static double bc_sin(double x) {
    const double PI = 3.14159265358979323846;
    const double TWO_PI = 6.28318530717958647692;
    while (x > PI) x -= TWO_PI;
    while (x < -PI) x += TWO_PI;
    double x2 = x * x;
    return x * (1.0 - x2 * (1.0/6.0 - x2 * (1.0/120.0 - x2 * (1.0/5040.0 - x2 / 362880.0))));
}
static double bc_cos(double x) { return bc_sin(x + 1.57079632679489661923); }

static void bc_init(bc_ctx_t *c, uint32_t *px, int w, int h) {
    c->px = px; c->w = w; c->h = h;
    c->sp = 0; c->npts = 0; c->nsubs = 0;
    c->st.fill_col = 0xFF000000u;
    c->st.stroke_col = 0xFF000000u;
    c->st.fill_grad.active = 0;
    c->st.fill_grad.nstops = 0;
    c->st.line_width = 1.0;
    c->st.alpha = 1.0;
    c->st.font_px = 14;
    c->st.m[0] = 1; c->st.m[1] = 0; c->st.m[2] = 0;
    c->st.m[3] = 1; c->st.m[4] = 0; c->st.m[5] = 0;
}

static inline void bc_xform(const bc_state_t *s, double x, double y, double *ox, double *oy) {
    *ox = s->m[0] * x + s->m[2] * y + s->m[4];
    *oy = s->m[1] * x + s->m[3] * y + s->m[5];
}

static inline uint32_t bc_blend(uint32_t dst, uint32_t src, double galpha) {
    uint32_t sa = (uint32_t)((double)((src >> 24) & 255u) * galpha + 0.5);
    if (sa == 0) return dst;
    if (sa >= 255) return (0xFF000000u | (src & 0x00FFFFFFu));
    uint32_t da = (dst >> 24) & 255u;
    if (da == 0) return (sa << 24) | (src & 0x00FFFFFFu);
    uint32_t inv = 255u - sa;
    uint32_t oa = sa + (da * inv + 127u) / 255u;
    if (oa == 0) return 0;
    uint32_t sr = (src >> 16) & 255u, sg = (src >> 8) & 255u, sb = src & 255u;
    uint32_t dr = (dst >> 16) & 255u, dg = (dst >> 8) & 255u, db = dst & 255u;
    uint32_t r = (sr * sa * 255u + dr * da * inv) / (oa * 255u);
    uint32_t g = (sg * sa * 255u + dg * da * inv) / (oa * 255u);
    uint32_t b = (sb * sa * 255u + db * da * inv) / (oa * 255u);
    if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
    return (oa << 24) | (r << 16) | (g << 8) | b;
}

static uint32_t bc_sample_fill(const bc_state_t *s, int x, int y) {
    if (!s->fill_grad.active || s->fill_grad.nstops == 0) return s->fill_col;
    const bc_grad_t *g = &s->fill_grad;
    double dx = g->x1 - g->x0, dy = g->y1 - g->y0;
    double d2 = dx * dx + dy * dy;
    double t = (d2 > 1e-9) ? (((double)x - g->x0) * dx + ((double)y - g->y0) * dy) / d2 : 0.0;
    if (t <= 0.0) return g->stops[0].col;
    if (t >= 1.0 || g->nstops == 1) return g->stops[g->nstops - 1].col;
    int i = 0;
    while (i + 1 < g->nstops && g->stops[i + 1].off < t) i++;
    if (i + 1 >= g->nstops) return g->stops[g->nstops - 1].col;
    double span = g->stops[i + 1].off - g->stops[i].off;
    double f = (span > 1e-6) ? (t - g->stops[i].off) / span : 0.0;
    if (f < 0) f = 0; if (f > 1) f = 1;
    uint32_t c0 = g->stops[i].col, c1 = g->stops[i + 1].col;
    uint32_t a = (uint32_t)(((c0 >> 24) & 255) * (1.0 - f) + ((c1 >> 24) & 255) * f + 0.5);
    uint32_t r = (uint32_t)(((c0 >> 16) & 255) * (1.0 - f) + ((c1 >> 16) & 255) * f + 0.5);
    uint32_t gg = (uint32_t)(((c0 >> 8) & 255) * (1.0 - f) + ((c1 >> 8) & 255) * f + 0.5);
    uint32_t b = (uint32_t)((c0 & 255) * (1.0 - f) + (c1 & 255) * f + 0.5);
    return (a << 24) | (r << 16) | (gg << 8) | b;
}

static inline void bc_put(bc_ctx_t *c, int x, int y, uint32_t col) {
    if (!c->px || x < 0 || y < 0 || x >= c->w || y >= c->h) return;
    c->px[(size_t)y * (size_t)c->w + (size_t)x] =
        bc_blend(c->px[(size_t)y * (size_t)c->w + (size_t)x], col, c->st.alpha);
}

static void bc_save(bc_ctx_t *c) {
    if (c->sp < BC_MAX_STACK) c->stack[c->sp++] = c->st;
}
static void bc_restore(bc_ctx_t *c) {
    if (c->sp > 0) c->st = c->stack[--c->sp];
}
static void bc_translate(bc_ctx_t *c, double tx, double ty) {
    c->st.m[4] += c->st.m[0] * tx + c->st.m[2] * ty;
    c->st.m[5] += c->st.m[1] * tx + c->st.m[3] * ty;
}
static void bc_scale(bc_ctx_t *c, double sx, double sy) {
    c->st.m[0] *= sx; c->st.m[1] *= sx;
    c->st.m[2] *= sy; c->st.m[3] *= sy;
}
static void bc_rotate(bc_ctx_t *c, double rad) {
    double cs = bc_cos(rad), sn = bc_sin(rad);
    double a = c->st.m[0], b = c->st.m[1], cc = c->st.m[2], d = c->st.m[3];
    c->st.m[0] = a * cs + cc * sn;  c->st.m[1] = b * cs + d * sn;
    c->st.m[2] = cc * cs - a * sn;  c->st.m[3] = d * cs - b * sn;
}

static void bc_begin_path(bc_ctx_t *c) { c->npts = 0; c->nsubs = 0; }
static void bc_add_pt(bc_ctx_t *c, double x, double y) {
    if (c->npts >= BC_MAX_PTS) return;
    double tx, ty; bc_xform(&c->st, x, y, &tx, &ty);
    c->px_pts[c->npts] = tx; c->py_pts[c->npts] = ty; c->npts++;
}
static void bc_move_to(bc_ctx_t *c, double x, double y) {
    if (c->nsubs < BC_MAX_SUBS) {
        c->sub_start[c->nsubs] = c->npts;
        c->sub_closed[c->nsubs] = 0;
        c->nsubs++;
    }
    bc_add_pt(c, x, y);
}
static void bc_line_to(bc_ctx_t *c, double x, double y) {
    if (c->nsubs == 0) bc_move_to(c, x, y);
    else bc_add_pt(c, x, y);
}
static void bc_close_path(bc_ctx_t *c) {
    if (c->nsubs > 0) c->sub_closed[c->nsubs - 1] = 1;
}
static void bc_rect(bc_ctx_t *c, double x, double y, double w, double h) {
    bc_move_to(c, x, y);
    bc_line_to(c, x + w, y);
    bc_line_to(c, x + w, y + h);
    bc_line_to(c, x, y + h);
    bc_close_path(c);
}
static void bc_arc(bc_ctx_t *c, double cx, double cy, double r, double a0, double a1, int ccw) {
    if (r < 0) r = -r;
    const double TWO_PI = 6.28318530717958647692;
    double sweep = a1 - a0;
    if (!ccw && sweep < 0) { while (sweep < 0) sweep += TWO_PI; }
    if (ccw && sweep > 0)  { while (sweep > 0) sweep -= TWO_PI; }
    if (sweep > TWO_PI) sweep = TWO_PI;
    if (sweep < -TWO_PI) sweep = -TWO_PI;
    int steps = (int)(r * 0.6) + 16;
    if (steps < 12) steps = 12;
    if (steps > 64) steps = 64;
    for (int i = 0; i <= steps; i++) {
        double ang = a0 + sweep * ((double)i / (double)steps);
        double x = cx + r * bc_cos(ang);
        double y = cy + r * bc_sin(ang);
        if (i == 0 && c->nsubs == 0) bc_move_to(c, x, y);
        else bc_line_to(c, x, y);
    }
}
static void bc_quad_to(bc_ctx_t *c, double cpx, double cpy, double x, double y) {
    if (c->npts == 0) { bc_move_to(c, x, y); return; }
    /* Approximate in local coords via inverse or direct world sampling */
    double x0 = c->px_pts[c->npts - 1], y0 = c->py_pts[c->npts - 1];
    double x1, y1, x2, y2;
    bc_xform(&c->st, cpx, cpy, &x1, &y1);
    bc_xform(&c->st, x, y, &x2, &y2);
    for (int i = 1; i <= 16 && c->npts < BC_MAX_PTS; i++) {
        double t = (double)i / 16.0, u = 1.0 - t;
        c->px_pts[c->npts] = u*u*x0 + 2*u*t*x1 + t*t*x2;
        c->py_pts[c->npts] = u*u*y0 + 2*u*t*y1 + t*t*y2;
        c->npts++;
    }
}
static void bc_bezier_to(bc_ctx_t *c, double cp1x, double cp1y, double cp2x, double cp2y, double x, double y) {
    if (c->npts == 0) { bc_move_to(c, x, y); return; }
    double x0 = c->px_pts[c->npts - 1], y0 = c->py_pts[c->npts - 1];
    double x1, y1, x2, y2, x3, y3;
    bc_xform(&c->st, cp1x, cp1y, &x1, &y1);
    bc_xform(&c->st, cp2x, cp2y, &x2, &y2);
    bc_xform(&c->st, x, y, &x3, &y3);
    for (int i = 1; i <= 20 && c->npts < BC_MAX_PTS; i++) {
        double t = (double)i / 20.0, u = 1.0 - t;
        c->px_pts[c->npts] = u*u*u*x0 + 3*u*u*t*x1 + 3*u*t*t*x2 + t*t*t*x3;
        c->py_pts[c->npts] = u*u*u*y0 + 3*u*u*t*y1 + 3*u*t*t*y2 + t*t*t*y3;
        c->npts++;
    }
}

/* Scanline polygon fill across all subpaths */
static void bc_fill(bc_ctx_t *c) {
    if (!c->px || c->npts < 3 || c->nsubs == 0) return;
    double miny = c->py_pts[0], maxy = c->py_pts[0];
    for (int i = 1; i < c->npts; i++) {
        if (c->py_pts[i] < miny) miny = c->py_pts[i];
        if (c->py_pts[i] > maxy) maxy = c->py_pts[i];
    }
    int y0 = (int)miny; if (y0 < 0) y0 = 0;
    int y1 = (int)maxy + 1; if (y1 >= c->h) y1 = c->h - 1;
    double xs[256];
    for (int y = y0; y <= y1; y++) {
        double sy = (double)y + 0.5;
        int nx = 0;
        for (int s = 0; s < c->nsubs; s++) {
            int st = c->sub_start[s];
            int en = (s + 1 < c->nsubs) ? c->sub_start[s + 1] : c->npts;
            int cnt = en - st;
            if (cnt < 3) continue;
            for (int i = 0; i < cnt && nx < 256; i++) {
                int j = (i + 1) % cnt;
                double ay = c->py_pts[st + i], by = c->py_pts[st + j];
                if ((ay <= sy && by > sy) || (by <= sy && ay > sy)) {
                    double ax = c->px_pts[st + i], bx = c->px_pts[st + j];
                    xs[nx++] = ax + (sy - ay) * (bx - ax) / (by - ay);
                }
            }
        }
        /* Insertion sort intersections */
        for (int i = 1; i < nx; i++) {
            double v = xs[i]; int j = i - 1;
            while (j >= 0 && xs[j] > v) { xs[j + 1] = xs[j]; j--; }
            xs[j + 1] = v;
        }
        for (int i = 0; i + 1 < nx; i += 2) {
            int xl = (int)(xs[i] + 0.5);
            int xr = (int)(xs[i + 1] - 0.5);
            if (xl < 0) xl = 0;
            if (xr >= c->w) xr = c->w - 1;
            for (int x = xl; x <= xr; x++)
                bc_put(c, x, y, bc_sample_fill(&c->st, x, y));
        }
    }
}

/* Thick line segment rasterizer */
static void bc_stroke_seg(bc_ctx_t *c, double x0, double y0, double x1, double y1) {
    double dx = x1 - x0, dy = y1 - y0;
    double adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int steps = (int)(adx > ady ? adx : ady) + 1;
    if (steps > 4096) steps = 4096;
    int rad = (int)(c->st.line_width * 0.5);
    int r2 = rad * rad;
    for (int i = 0; i <= steps; i++) {
        double t = (double)i / (double)steps;
        int px = (int)(x0 + dx * t + 0.5);
        int py = (int)(y0 + dy * t + 0.5);
        if (rad <= 0) {
            bc_put(c, px, py, c->st.stroke_col);
        } else {
            for (int oy = -rad; oy <= rad; oy++)
                for (int ox = -rad; ox <= rad; ox++)
                    if (ox * ox + oy * oy <= r2 + rad)
                        bc_put(c, px + ox, py + oy, c->st.stroke_col);
        }
    }
}

static void bc_stroke(bc_ctx_t *c) {
    if (!c->px || c->npts < 2) return;
    for (int s = 0; s < c->nsubs; s++) {
        int st = c->sub_start[s];
        int en = (s + 1 < c->nsubs) ? c->sub_start[s + 1] : c->npts;
        for (int i = st; i + 1 < en; i++)
            bc_stroke_seg(c, c->px_pts[i], c->py_pts[i], c->px_pts[i + 1], c->py_pts[i + 1]);
        if (c->sub_closed[s] && en - st >= 2)
            bc_stroke_seg(c, c->px_pts[en - 1], c->py_pts[en - 1], c->px_pts[st], c->py_pts[st]);
    }
}

static void bc_clear_rect(bc_ctx_t *c, double x, double y, double w, double h) {
    if (!c->px) return;
    double tx0, ty0, tx1, ty1;
    bc_xform(&c->st, x, y, &tx0, &ty0);
    bc_xform(&c->st, x + w, y + h, &tx1, &ty1);
    int x0 = (int)(tx0 < tx1 ? tx0 : tx1), x1 = (int)(tx0 > tx1 ? tx0 : tx1);
    int y0 = (int)(ty0 < ty1 ? ty0 : ty1), y1 = (int)(ty0 > ty1 ? ty0 : ty1);
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > c->w) x1 = c->w; if (y1 > c->h) y1 = c->h;
    for (int py = y0; py < y1; py++)
        for (int px = x0; px < x1; px++)
            c->px[(size_t)py * (size_t)c->w + (size_t)px] = 0x00000000u;
}

static void bc_fill_rect(bc_ctx_t *c, double x, double y, double w, double h) {
    int sn = c->npts, ss = c->nsubs;
    bc_begin_path(c);
    bc_rect(c, x, y, w, h);
    bc_fill(c);
    c->npts = sn; c->nsubs = ss;
}

static void bc_stroke_rect(bc_ctx_t *c, double x, double y, double w, double h) {
    int sn = c->npts, ss = c->nsubs;
    bc_begin_path(c);
    bc_rect(c, x, y, w, h);
    bc_stroke(c);
    c->npts = sn; c->nsubs = ss;
}

/* Compact 5x7 ASCII glyph bitmap for canvas fillText (zero external deps) */
static const uint8_t BC_FONT5X7[96][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},{0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},{0x7F,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},{0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},{0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7F,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},{0x10,0x08,0x08,0x10,0x08},{0x00,0x00,0x00,0x00,0x00}
};

static void bc_fill_text(bc_ctx_t *c, const char *txt, double x, double y) {
    if (!c->px || !txt) return;
    double tx, ty; bc_xform(&c->st, x, y, &tx, &ty);
    int scale = c->st.font_px / 7;
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;
    int cx = (int)(tx + 0.5);
    int cy = (int)(ty + 0.5) - 7 * scale;
    for (int i = 0; txt[i]; i++) {
        unsigned char ch = (unsigned char)txt[i];
        if (ch < 32 || ch > 127) ch = '?';
        const uint8_t *g = BC_FONT5X7[ch - 32];
        for (int col = 0; col < 5; col++) {
            uint8_t bits = g[col];
            for (int row = 0; row < 7; row++) {
                if (bits & (1u << row)) {
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++)
                            bc_put(c, cx + col * scale + sx, cy + row * scale + sy,
                                   bc_sample_fill(&c->st, cx + col * scale + sx, cy + row * scale + sy));
                }
            }
        }
        cx += 6 * scale;
    }
}

#endif /* ARCTIAN_BROWSER_CANVAS_H */
