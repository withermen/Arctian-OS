/**
 * quickctl.c - Hizli Ayarlar + OSD + bildirim merkezi + perdeler
 *
 * Gorev cubugundaki zil (bildirim) ve ayarlar (hizli ayarlar) dugmelerine
 * baglidir.  - Hizli Ayarlar: ses, parlaklik, Wi-Fi, ucak modu, gece modu,
 * ekran goruntusu  - Ses/parlaklik degisince 1.5 sn OSD gostergesi  -
 * Bildirim merkezi: arctian_notify ile gelen son 10 bildirim + Temizle  -
 * Yazilimsal parlaklik karartmasi ve gece (amber) perdesi: tiklanamaz,
 * en ustte; pencere/gorev cubugu uzerinde gorunur.
 * Kalicilik: settings.cfg kv — bright, night, airplane, wifi_on.
 */
#include "dsk.h"

#define QC_N      10        /* bildirim yiginı kapasitesi */
#define QC_ROW_MAX 4        /* satir basina kopya siniri */

/* ---- Bildirim yiginı (en yeni 0. sira) ---- */
static char s_nt[QC_N][24];
static char s_nm[QC_N][80];
static int  s_n = 0;
static int  s_unread = 0;

/* ---- Nesneler ---- */
static lv_obj_t *s_qs;          /* hizli ayarlar paneli */
static lv_obj_t *s_nc;          /* bildirim merkezi */
static lv_obj_t *s_nc_list;     /* bildirim listesi govdesi */
static lv_obj_t *s_osd;         /* OSD kutusu */
static lv_obj_t *s_osd_lbl;
static lv_obj_t *s_osd_bar;
static lv_timer_t *s_osd_t;
static lv_obj_t *s_dim;         /* parlaklik karartma perdesi */
static lv_obj_t *s_night;       /* gece (amber) perdesi */

/* Panel ici etiketler (durum yazilari) */
static lv_obj_t *s_wifi_lbl, *s_air_lbl, *s_night_lbl, *s_ses_lbl, *s_par_lbl;
static lv_obj_t *s_ses_sl, *s_par_sl;

static void scpy24(char *d, int cap, const char *s) {
    int i = 0;
    if (!s) { d[0] = 0; return; }
    for (; s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}

static void qc_nc_fill(void);   /* ileri bildirim (liste doldurma) */


/* ------------------------------------------------------------------ */
/* Bildirim yiginı                                                     */
/* ------------------------------------------------------------------ */
void qc_notify_add(const char *title, const char *msg) {
    /* kaydir, en yeni 0. sira */
    for (int i = QC_N - 1; i > 0; i--) {
        scpy24(s_nt[i], sizeof(s_nt[i]), s_nt[i - 1]);
        scpy24(s_nm[i], sizeof(s_nm[i]), s_nm[i - 1]);
    }
    scpy24(s_nt[0], sizeof(s_nt[0]), title ? title : "Bildirim");
    scpy24(s_nm[0], sizeof(s_nm[0]), msg ? msg : "");
    if (s_n < QC_N) s_n++;
    /* Okunmamis sayaci: zil uzerinde rozet gosterilir.      */
    s_unread++;
    tb_refresh_bell_badge();

    /* merkez aciksa listeyi tazele */
    if (s_nc && !lv_obj_has_flag(s_nc, LV_OBJ_FLAG_HIDDEN)) qc_nc_fill();
}

int qc_unread_count(void) { return s_unread; }
void qc_unread_clear(void) { s_unread = 0; tb_refresh_bell_badge(); }


/* ------------------------------------------------------------------ */
/* Perdeler: parlaklik karartmasi + gece modu                          */
/* ------------------------------------------------------------------ */
static void qc_apply_overlays_bright(int bright) {
    int night = kv_get_int("night", 0);
    if (bright < 30) bright = 30;
    if (bright > 100) bright = 100;

    int hw_ok = 0;
    if (g_asi && g_asi->power && g_asi->power->brightness_set)
        hw_ok = (g_asi->power->brightness_set(bright) == 0);

    int W = (g_asi && g_asi->gfx && g_asi->gfx->width) ? g_asi->gfx->width() : 1920;
    int H = (g_asi && g_asi->gfx && g_asi->gfx->height) ? g_asi->gfx->height() : 1080;

    if (s_dim) {
        lv_obj_set_size(s_dim, W, H);
        lv_obj_set_pos(s_dim, 0, 0);
        /* karartma: donanim parlakligi desteklenmiyorsa yazilimsal perde */
        int opa = hw_ok ? 0 : (((100 - bright) * 255) / 100);
        lv_obj_set_style_bg_opa(s_dim, opa, 0);
        if (opa > 0) lv_obj_clear_flag(s_dim, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_dim, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_night) {
        lv_obj_set_size(s_night, W, H);
        lv_obj_set_pos(s_night, 0, 0);
        if (night) lv_obj_clear_flag(s_night, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_night, LV_OBJ_FLAG_HIDDEN);
    }

    /* Perdeler qc_reinit() icinde lv_obj_move_background() ile DUVAR KAGIDININ
     * ARKASINA aliniyordu (z-sirasi: arka plan = en alt); parlaklik karartmasi
     * ve gece modu bu yuzden hic gorunmuyordu. Artik ETKIN perdeler en one
     * alinir. Tiklama tehlikesi yok: mk_perde() perdeleri LV_OBJ_FLAG_CLICKABLE
     * disinda birakir, lv_obj_hit_test() tiklanamayan nesneyi false dondurur ve
     * tiklamalar alttaki masaustu/pencerelere gecer.
     *
     * Artik ETKIN perdeler en one alinir. Guvenlik: perdeler mk_perde()
     * ile LV_OBJ_FLAG_CLICKABLE disi birakilmistir; lv_obj_hit_test()
     * (lv_obj_pos.c:1236) tıklanamayan nesneyi false donecegi icin
     * tiklamalar alttaki masaustu/pencerelere gecer.
     *
     * Sira onemli: gece (amber) once, karartma (siyah) sonra — boylece
     * karartma ustte kalir. Gorunur OSD her zaman en ustte tutulur. */
    if (s_night && night) lv_obj_move_foreground(s_night);
    if (s_dim && !hw_ok && bright < 100) lv_obj_move_foreground(s_dim);
    if (s_osd && !lv_obj_has_flag(s_osd, LV_OBJ_FLAG_HIDDEN))
        lv_obj_move_foreground(s_osd);
}

void qc_apply_overlays(void) {
    qc_apply_overlays_bright(kv_get_int("bright", 100));
}


/* ------------------------------------------------------------------ */
/* OSD (1.5 sn)                                                        */
/* ------------------------------------------------------------------ */
static void qc_osd_hide_cb(lv_timer_t *t) {
    if (s_osd) lv_obj_add_flag(s_osd, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(t);            /* gizlenince dur (yeniden kullanilabilir) */
}


void qc_osd(const char *label, int pct) {
    if (!s_osd || !s_osd_lbl || !s_osd_bar) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    char b[48];
    int i = 0;
    const char *s = label ? label : "";
    while (s[i] && i < 30) { b[i] = s[i]; i++; }
    b[i++] = ' ';
    b[i++] = '%';
    int v = pct, k = 0;
    char tmp[4];
    if (v == 0) tmp[k++] = '0';
    while (v > 0 && k < 3) { tmp[k++] = (char)('0' + (v % 10)); v /= 10; }
    while (k > 0 && i < 46) b[i++] = tmp[--k];
    b[i] = 0;
    lv_label_set_text(s_osd_lbl, b);
    lv_slider_set_value(s_osd_bar, pct, LV_ANIM_OFF);
    lv_obj_clear_flag(s_osd, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_osd);
    if (!s_osd_t) {
        s_osd_t = lv_timer_create(qc_osd_hide_cb, 1500, 0);
        lv_timer_pause(s_osd_t);        /* gosterilince tekrar calisir */
    }
    lv_timer_reset(s_osd_t);            /* sayaci sifirla */
    lv_timer_resume(s_osd_t);
}


/* ------------------------------------------------------------------ */
/* Ekran goruntusu (PNG -> C:/ekran_XX.png, foto ile uyumlu)          */
/* ------------------------------------------------------------------ */
typedef struct {
    void     *fh;
    uint32_t *snap;          /* w*h piksellik framebuffer kopyasi */
    int       w, h;
    int       ok;
    char      name[24];
} qc_shot_t;
static qc_shot_t s_shot;
static int       s_shot_busy;

static void png_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

static uint32_t png_crc32(const uint8_t *buf, uint32_t len) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        c ^= buf[i];
        for (int b = 0; b < 8; b++)
            c = (c & 1u) ? ((c >> 1) ^ 0xEDB88320u) : (c >> 1);
    }
    return c ^ 0xFFFFFFFFu;
}

static void qc_shot_work(void *u) {
    qc_shot_t *s = (qc_shot_t *)u;
    s->ok = 0;
    if (!g_asi || !g_asi->mem || !g_asi->mem->alloc || !g_asi->fs || !g_asi->fs->write) return;

    uint32_t w = (uint32_t)s->w;
    uint32_t h = (uint32_t)s->h;
    uint32_t row_bytes = 1u + w * 3u;
    uint32_t raw_size = h * row_bytes;
    uint32_t nblocks = (raw_size + 65534u) / 65535u;
    if (nblocks == 0) nblocks = 1;
    uint32_t zlib_size = 2u + nblocks * 5u + raw_size + 4u;
    uint32_t total_size = 8u + 25u + (12u + zlib_size) + 12u;

    uint8_t *png = (uint8_t *)g_asi->mem->alloc(total_size);
    if (!png) return;

    uint8_t *p = png;
    /* PNG imza */
    static const uint8_t sig[8] = { 137u, 80u, 78u, 71u, 13u, 10u, 26u, 10u };
    for (int i = 0; i < 8; i++) *p++ = sig[i];

    /* IHDR (13 bayt, 8-bit RGB non-interlaced) */
    png_be32(p, 13u); p += 4;
    uint8_t *ihdr_type = p;
    *p++ = 'I'; *p++ = 'H'; *p++ = 'D'; *p++ = 'R';
    png_be32(p, w); p += 4;
    png_be32(p, h); p += 4;
    *p++ = 8u; /* bit depth */
    *p++ = 2u; /* color type: RGB */
    *p++ = 0u; /* compression */
    *p++ = 0u; /* filter */
    *p++ = 0u; /* interlace */
    png_be32(p, png_crc32(ihdr_type, 17u)); p += 4;

    /* IDAT (zlib + DEFLATE stored bloklar) */
    png_be32(p, zlib_size); p += 4;
    uint8_t *idat_type = p;
    *p++ = 'I'; *p++ = 'D'; *p++ = 'A'; *p++ = 'T';
    *p++ = 0x78u; /* CMF: deflate, 32K pencere */
    *p++ = 0x01u; /* FLG: 0x7801 % 31 == 0 */

    uint32_t raw_rem = raw_size;
    uint32_t blk_rem = 0;
    uint32_t s1 = 1u, s2 = 0u;

    for (uint32_t y = 0; y < h; y++) {
        const uint32_t *src = s->snap + y * w;
        for (uint32_t col = 0; col <= w; col++) {
            uint8_t bytes[3];
            uint32_t cnt;
            if (col == 0) {
                bytes[0] = 0u; /* filter: None */
                cnt = 1u;
            } else {
                uint32_t px = src[col - 1u];
                bytes[0] = (uint8_t)((px >> 16) & 0xFFu);
                bytes[1] = (uint8_t)((px >> 8) & 0xFFu);
                bytes[2] = (uint8_t)(px & 0xFFu);
                cnt = 3u;
            }
            for (uint32_t bi = 0; bi < cnt; bi++) {
                if (blk_rem == 0) {
                    uint32_t chunk = (raw_rem > 65535u) ? 65535u : raw_rem;
                    uint16_t nlen = (uint16_t)(chunk ^ 0xFFFFu);
                    *p++ = (chunk == raw_rem) ? 0x01u : 0x00u;
                    *p++ = (uint8_t)(chunk & 0xFFu);
                    *p++ = (uint8_t)((chunk >> 8) & 0xFFu);
                    *p++ = (uint8_t)(nlen & 0xFFu);
                    *p++ = (uint8_t)((nlen >> 8) & 0xFFu);
                    blk_rem = chunk;
                }
                uint8_t b = bytes[bi];
                *p++ = b;
                s1 += b; if (s1 >= 65521u) s1 -= 65521u;
                s2 += s1; if (s2 >= 65521u) s2 -= 65521u;
                blk_rem--;
                raw_rem--;
            }
        }
    }
    png_be32(p, (s2 << 16) | s1); p += 4;
    png_be32(p, png_crc32(idat_type, 4u + zlib_size)); p += 4;

    /* IEND */
    png_be32(p, 0u); p += 4;
    *p++ = 'I'; *p++ = 'E'; *p++ = 'N'; *p++ = 'D';
    png_be32(p, 0xAE426082u); p += 4;

    if (g_asi->fs->write(s->fh, png, total_size) == 0) s->ok = 1;
    if (g_asi->mem->free) g_asi->mem->free(png);
}

static void qc_shot_done(int status, void *u) {
    (void)status;
    qc_shot_t *s = (qc_shot_t *)u;
    if (s->snap && g_asi->mem && g_asi->mem->free) g_asi->mem->free(s->snap);
    s->snap = 0;
    s_shot_busy = 0;
    if (s->ok) {
        qc_osd("Ekran görüntüsü", 100);
        arctian_notify("Ekran görüntüsü", s->name);
    } else {
        qc_osd("Ekran görüntüsü", 0);
    }
}

static void qc_screenshot(void) {
    if (!g_asi || !g_asi->gfx || !g_asi->fs || !g_asi->fs->create) return;
    if (s_shot_busy) { qc_osd("Görüntü hazırlanıyor", 0); return; }
    uint32_t *fb = g_asi->gfx->framebuffer();
    int w = g_asi->gfx->width(), h = g_asi->gfx->height();
    if (!fb || w < 1 || h < 1 || w > 3840 || h > 2160) return;

    char name[24];
    void *fh = 0;
    for (int i = 1; i < 100 && !fh; i++) {
        int k = 0;
        const char *pre = "C:/ekran_";
        while (*pre) name[k++] = *pre++;
        name[k++] = (char)('0' + (i / 10));
        name[k++] = (char)('0' + (i % 10));
        name[k++] = '.'; name[k++] = 'p'; name[k++] = 'n'; name[k++] = 'g';
        name[k] = 0;
        int exists = 0;
        if (g_asi->fs->find && g_asi->fs->find(0, name) != 0) exists = 1;
        else if (g_asi->fs->stat) {
            asi_stat_t st;
            if (g_asi->fs->stat(name, &st) == 0) exists = 1;
        }
        if (!exists) fh = g_asi->fs->create(0, name, 0);
    }
    if (!fh) { qc_osd("Ekran görüntüsü", 0); return; }

    uint32_t npix = (uint32_t)w * (uint32_t)h;
    uint32_t *snap = 0;
    if (g_asi->mem && g_asi->mem->alloc)
        snap = (uint32_t *)g_asi->mem->alloc(npix * 4u);
    if (!snap) { qc_osd("Ekran görüntüsü", 0); return; }
    for (uint32_t i = 0; i < npix; i++) snap[i] = fb[i];

    s_shot.fh = fh;
    s_shot.snap = snap;
    s_shot.w = w;
    s_shot.h = h;
    s_shot.ok = 0;
    int l = 0;
    while (name[l] && l < (int)sizeof(s_shot.name) - 1) { s_shot.name[l] = name[l]; l++; }
    s_shot.name[l] = 0;

    if (g_asi->net && g_asi->net->submit &&
        !(g_asi->net->busy && g_asi->net->busy()) &&
        g_asi->net->submit(qc_shot_work, qc_shot_done, &s_shot) == 0) {
        s_shot_busy = 1;
        return;
    }
    qc_shot_work(&s_shot);
    qc_shot_done(0, &s_shot);
}


/* ------------------------------------------------------------------ */
/* Hizli Ayarlar paneli                                                */
/* ------------------------------------------------------------------ */
static void qc_qs_refresh(void);   /* ileri bildirim */

static void ses_cb(lv_event_t *e) {
    lv_obj_t *sl = lv_event_get_target(e);
    int v = lv_slider_get_value(sl);
    if (g_asi && g_asi->audio && g_asi->audio->set_volume) g_asi->audio->set_volume(v);
    if (s_ses_lbl) {
        char b[8]; int k = 0, t = v;
        if (t == 0) b[k++] = '0';
        while (t > 0 && k < 4) { b[k++] = (char)('0' + (t % 10)); t /= 10; }
        char r[8]; int m = 0;
        r[m++] = '%';
        while (k > 0 && m < 7) r[m++] = b[--k];
        r[m] = 0;
        lv_label_set_text(s_ses_lbl, r);
    }
    qc_osd("Ses", v);
}


static void ses_end_cb(lv_event_t *e) {
    (void)e;
    /* surme bitince kaliciga yaz (her adimda yazma yapmamak icin) */
    int v = s_ses_sl ? lv_slider_get_value(s_ses_sl) : 50;
    if (g_asi && g_asi->audio && g_asi->audio->set_volume) g_asi->audio->set_volume(v);
    char b[8]; itoa_buf(v, b, sizeof(b));
    kv_set("vol", b);
}


static void parlak_cb(lv_event_t *e) {
    lv_obj_t *sl = lv_event_get_target(e);
    int v = lv_slider_get_value(sl);
    if (s_par_lbl) {
        char b[8]; int k = 0, t = v;
        if (t == 0) b[k++] = '0';
        while (t > 0 && k < 4) { b[k++] = (char)('0' + (t % 10)); t /= 10; }
        char r[8]; int m = 0;
        r[m++] = '%';
        while (k > 0 && m < 7) r[m++] = b[--k];
        r[m] = 0;
        lv_label_set_text(s_par_lbl, r);
    }
    qc_apply_overlays_bright(v);       /* aninda karart */
    qc_osd("Parlaklık", v);
}


static void parlak_end_cb(lv_event_t *e) {
    (void)e;
    int v = s_par_sl ? lv_slider_get_value(s_par_sl) : 100;
    char b[8]; itoa_buf(v, b, sizeof(b));
    kv_set("bright", b);
    /* qc_apply_overlays() parlakligi kv_get_int("bright", 100) ile okur; bu geri
     * cagri ise kv'ye yazar. Ikisi ayri kaldiginda perde daima "bright=100"
     * ile kapali kaliyordu, bu yuzden deger yazildiktan hemen sonra perde
     * yeniden uygulanir. Surukleme sirasinda disk yazimindan kacinmak icin deger
     * yalnizca birakmada kaydedilir; OSD aninda guncellenir. */
    qc_apply_overlays();
}


static void wifi_cb(lv_event_t *e) {
    (void)e;
    if (kv_get_int("airplane", 0)) {
        qc_osd("Uçak modu açık", 0);
        return;
    }
    int on = kv_get_int("wifi_on", 1) ? 0 : 1;      /* tersine cevir */
    kv_set("wifi_on", on ? "1" : "0");
    if (!on && g_asi && g_asi->net && g_asi->net->wifi_disconnect)
        g_asi->net->wifi_disconnect();
    qc_qs_refresh();
}


static void air_cb(lv_event_t *e) {
    (void)e;
    int on = kv_get_int("airplane", 0) ? 0 : 1;
    kv_set("airplane", on ? "1" : "0");
    if (on) {
        kv_set("wifi_on", "0");
        if (g_asi && g_asi->net && g_asi->net->wifi_disconnect)
            g_asi->net->wifi_disconnect();
    }
    qc_qs_refresh();
}


static void night_cb(lv_event_t *e) {
    (void)e;
    int on = kv_get_int("night", 0) ? 0 : 1;
    kv_set("night", on ? "1" : "0");
    qc_apply_overlays();
    qc_qs_refresh();
}


static void shot_cb(lv_event_t *e) { (void)e; qc_screenshot(); }

/* Satir: sol etiket + sagdurum yazisi */
static lv_obj_t *qc_row(lv_obj_t *p, const char *title, lv_obj_t **lbl) {
    lv_obj_t *r = lv_obj_create(p);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), 34);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(r);
    lv_label_set_text(l, title);
    lv_obj_set_style_text_color(l, lv_color_hex(ADL_TEXT), 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_t *st = lv_label_create(r);
    lv_label_set_text(st, "");
    lv_obj_set_style_text_color(st, lv_color_hex(ADL_ACCENT), 0);
    lv_obj_align(st, LV_ALIGN_RIGHT_MID, -4, 0);
    if (lbl) *lbl = st;
    return r;
}


static void qc_qs_refresh(void) {
    if (!s_qs) return;
    int wifi = kv_get_int("wifi_on", 1);
    int air = kv_get_int("airplane", 0);
    int night = kv_get_int("night", 0);
    if (air) wifi = 0;

    if (s_wifi_lbl)
        lv_label_set_text(s_wifi_lbl, air ? "Uçak modu" : (wifi ? "Açık" : "Kapalı"));
    if (s_air_lbl)
        lv_label_set_text(s_air_lbl, air ? "Açık" : "Kapalı");
    if (s_night_lbl)
        lv_label_set_text(s_night_lbl, night ? "Açık" : "Kapalı");

    if (s_ses_sl) {
        int v = (g_asi && g_asi->audio && g_asi->audio->volume)
                ? g_asi->audio->volume() : kv_get_int("vol", 50);
        lv_slider_set_value(s_ses_sl, v, LV_ANIM_OFF);
        if (s_ses_lbl) {
            char b[8]; int k = 0, t = v;
            if (t == 0) b[k++] = '0';
            while (t > 0 && k < 4) { b[k++] = (char)('0' + (t % 10)); t /= 10; }
            char r[8]; int m = 0; r[m++] = '%';
            while (k > 0 && m < 7) r[m++] = b[--k];
            r[m] = 0;
            lv_label_set_text(s_ses_lbl, r);
        }
    }
    if (s_par_sl) {
        int v = kv_get_int("bright", 100);
        lv_slider_set_value(s_par_sl, v, LV_ANIM_OFF);
        if (s_par_lbl) {
            char b[8]; int k = 0, t = v;
            if (t == 0) b[k++] = '0';
            while (t > 0 && k < 4) { b[k++] = (char)('0' + (t % 10)); t /= 10; }
            char r[8]; int m = 0; r[m++] = '%';
            while (k > 0 && m < 7) r[m++] = b[--k];
            r[m] = 0;
            lv_label_set_text(s_par_lbl, r);
        }
    }
}


static void qc_build_qs(void) {
    int W = 300, H = 356;
    s_qs = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_qs);
    lv_obj_set_size(s_qs, W, H);
    lv_obj_set_style_radius(s_qs, 14, 0);
    lv_obj_set_style_bg_color(s_qs, lv_color_hex(ADL_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_qs, 245, 0);
    lv_obj_set_style_border_width(s_qs, 1, 0);
    lv_obj_set_style_border_color(s_qs, lv_color_hex(ADL_BORDER), 0);
    lv_obj_set_style_shadow_width(s_qs, 18, 0);
    lv_obj_set_style_shadow_color(s_qs, lv_color_hex(0), 0);
    lv_obj_set_style_shadow_opa(s_qs, 130, 0);
    lv_obj_set_style_pad_all(s_qs, 14, 0);
    lv_obj_set_style_pad_row(s_qs, 6, 0);
    /* LVGL'de varsayilan layout yoktur: flex flow kurulmazsa tum cocuklar
     * (baslik + 6 satir + 2 slider = 9 oge) (0,0)'a yigilir ve panelde tek bir
     * metin bandi gorunur. Bu yuzden satirlar dikey akisla dizilir. */
    lv_obj_set_flex_flow(s_qs, LV_FLEX_FLOW_COLUMN);
    lv_obj_align(s_qs, LV_ALIGN_BOTTOM_RIGHT, -24, -(TB_BH + TB_Y_OFF + 16));
    lv_obj_add_flag(s_qs, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *hd = lv_label_create(s_qs);
    lv_label_set_text(hd, "Hızlı Ayarlar");
    lv_obj_set_style_text_color(hd, lv_color_hex(ADL_ACCENT), 0);

    /* Ses */
    lv_obj_t *r1 = qc_row(s_qs, "Ses", &s_ses_lbl);
    (void)r1;
    s_ses_sl = lv_slider_create(s_qs);
    /* Yukseklik bilincli 26 yapildi: flex flow'dan once slider dogal
     * yuksekligi kontrolsuzdu ve 6 satir + 2 slider + 8 bosluk panelin
     * 328px'lik ic alaniyla sigmaya bagliydi. 26 ile toplam 320 (varsayilan
     * 20px baslik) -> panel tam sigar, tasma olmaz. */
    lv_obj_set_size(s_ses_sl, LV_PCT(100), 26);
    lv_slider_set_range(s_ses_sl, 0, 100);
    lv_obj_add_event_cb(s_ses_sl, ses_cb, LV_EVENT_VALUE_CHANGED, 0);
    lv_obj_add_event_cb(s_ses_sl, ses_end_cb, LV_EVENT_RELEASED, 0);

    /* Parlaklik */
    lv_obj_t *r2 = qc_row(s_qs, "Parlaklık", &s_par_lbl);
    (void)r2;
    s_par_sl = lv_slider_create(s_qs);
    lv_obj_set_size(s_par_sl, LV_PCT(100), 26);
    lv_slider_set_range(s_par_sl, 30, 100);      /* en koyu %70 karartma */
    lv_obj_add_event_cb(s_par_sl, parlak_cb, LV_EVENT_VALUE_CHANGED, 0);
    lv_obj_add_event_cb(s_par_sl, parlak_end_cb, LV_EVENT_RELEASED, 0);

    /* Wi-Fi */
    lv_obj_t *rw = qc_row(s_qs, "Wi-Fi", &s_wifi_lbl);
    lv_obj_add_flag(rw, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(rw, wifi_cb, LV_EVENT_CLICKED, 0);

    /* Ucak modu */
    lv_obj_t *ra = qc_row(s_qs, "Uçak modu", &s_air_lbl);
    lv_obj_add_flag(ra, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ra, air_cb, LV_EVENT_CLICKED, 0);

    /* Gece modu */
    lv_obj_t *rn = qc_row(s_qs, "Gece modu", &s_night_lbl);
    lv_obj_add_flag(rn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(rn, night_cb, LV_EVENT_CLICKED, 0);

    /* Ekran goruntusu */
    lv_obj_t *rs = lv_obj_create(s_qs);
    lv_obj_remove_style_all(rs);
    lv_obj_set_size(rs, LV_PCT(100), 34);
    lv_obj_remove_flag(rs, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(rs, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(rs, shot_cb, LV_EVENT_CLICKED, 0);
    lv_obj_t *sl2 = lv_label_create(rs);
    lv_label_set_text(sl2, "Ekran görüntüsü al");
    lv_obj_set_style_text_color(sl2, lv_color_hex(ADL_TEXT), 0);
    lv_obj_center(sl2);

    qc_qs_refresh();
}


/* ------------------------------------------------------------------ */
/* Bildirim merkezi                                                    */
/* ------------------------------------------------------------------ */
void qc_nc_fill(void) {
    if (!s_nc_list) return;
    lv_obj_clean(s_nc_list);
    if (s_n == 0) {
        lv_obj_t *e = lv_label_create(s_nc_list);
        lv_label_set_text(e, "Bildirim yok");
        lv_obj_set_style_text_color(e, lv_color_hex(ADL_TEXT_DIM), 0);
        return;
    }
    for (int i = 0; i < s_n; i++) {
        lv_obj_t *c = lv_obj_create(s_nc_list);
        lv_obj_remove_style_all(c);
        lv_obj_set_size(c, LV_PCT(100), 48);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(c, lv_color_hex(ADL_SURFACE_2), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(c, 8, 0);
        lv_obj_t *t = lv_label_create(c);
        lv_label_set_text(t, s_nt[i]);
        lv_obj_set_style_text_color(t, lv_color_hex(ADL_ACCENT), 0);
        lv_obj_set_pos(t, 8, 4);
        lv_obj_set_width(t, 250);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_t *m = lv_label_create(c);
        lv_label_set_text(m, s_nm[i]);
        lv_obj_set_style_text_color(m, lv_color_hex(ADL_TEXT_DIM), 0);
        lv_obj_set_pos(m, 8, 26);
        lv_obj_set_width(m, 250);
        lv_label_set_long_mode(m, LV_LABEL_LONG_DOT);
    }
}


static void qc_nc_clear_cb(lv_event_t *e) {
    (void)e;
    s_n = 0;
    qc_nc_fill();
}


static void qc_build_nc(void) {
    int W = 300, H = 320;
    s_nc = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_nc);
    lv_obj_set_size(s_nc, W, H);
    lv_obj_set_style_radius(s_nc, 14, 0);
    lv_obj_set_style_bg_color(s_nc, lv_color_hex(ADL_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_nc, 245, 0);
    lv_obj_set_style_border_width(s_nc, 1, 0);
    lv_obj_set_style_border_color(s_nc, lv_color_hex(ADL_BORDER), 0);
    lv_obj_set_style_shadow_width(s_nc, 18, 0);
    lv_obj_set_style_shadow_color(s_nc, lv_color_hex(0), 0);
    lv_obj_set_style_shadow_opa(s_nc, 130, 0);
    lv_obj_set_style_pad_all(s_nc, 12, 0);
    lv_obj_set_style_pad_row(s_nc, 8, 0);
    /* Bildirim govdesi de dikey flex akis kullanir; kurulmazsa baslik, liste ve
     * "Temizle" dugmesi (0,0)'a yigilir. Icerik 20+8+230+8+34=300 px, 296 px'lik
     * alani 4 px asar; panelin 12 px'lik alt boslugu bunu karsilar. */
    lv_obj_set_flex_flow(s_nc, LV_FLEX_FLOW_COLUMN);
    lv_obj_align(s_nc, LV_ALIGN_BOTTOM_RIGHT, -24, -(TB_BH + TB_Y_OFF + 16));
    lv_obj_add_flag(s_nc, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *hd = lv_label_create(s_nc);
    lv_label_set_text(hd, "Bildirimler");
    lv_obj_set_style_text_color(hd, lv_color_hex(ADL_ACCENT), 0);

    s_nc_list = lv_obj_create(s_nc);
    lv_obj_remove_style_all(s_nc_list);
    lv_obj_set_size(s_nc_list, LV_PCT(100), 230);
    lv_obj_remove_flag(s_nc_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_row(s_nc_list, 6, 0);
    lv_obj_set_flex_flow(s_nc_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_nc_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_nc_list, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *clr = lv_button_create(s_nc);
    lv_obj_set_size(clr, LV_PCT(100), 34);
    lv_obj_set_style_radius(clr, 8, 0);
    lv_obj_set_style_bg_color(clr, lv_color_hex(ADL_SURFACE_2), 0);
    lv_obj_t *cl = lv_label_create(clr);
    lv_label_set_text(cl, "Temizle");
    lv_obj_set_style_text_color(cl, lv_color_hex(ADL_TEXT), 0);
    lv_obj_center(cl);
    lv_obj_add_event_cb(clr, qc_nc_clear_cb, LV_EVENT_CLICKED, 0);

    qc_nc_fill();
}


/* ------------------------------------------------------------------ */
/* Ac/kapa                                                             */
/* ------------------------------------------------------------------ */
void qc_show_quick(void) {
    if (!s_qs) return;
    if (lv_obj_has_flag(s_qs, LV_OBJ_FLAG_HIDDEN)) {
        qc_qs_refresh();
        lv_obj_clear_flag(s_qs, LV_OBJ_FLAG_HIDDEN);
        if (s_nc) lv_obj_add_flag(s_nc, LV_OBJ_FLAG_HIDDEN);   /* ayni anda tek */
        lv_obj_move_foreground(s_qs);
    } else {
        lv_obj_add_flag(s_qs, LV_OBJ_FLAG_HIDDEN);
    }
}


void qc_show_notif(void) {
    if (!s_nc) return;
    if (lv_obj_has_flag(s_nc, LV_OBJ_FLAG_HIDDEN)) {
        qc_nc_fill();
        lv_obj_clear_flag(s_nc, LV_OBJ_FLAG_HIDDEN);
        if (s_qs) lv_obj_add_flag(s_qs, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_nc);
    } else {
        lv_obj_add_flag(s_nc, LV_OBJ_FLAG_HIDDEN);
    }
}


/* ------------------------------------------------------------------ */
/* Kurulum (boot + kabuk yeniden kurulumu)                             */
/* ------------------------------------------------------------------ */
static lv_obj_t *mk_perde(uint32_t color) {
    lv_obj_t *o = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);      /* tiklamalar altina gecer */
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, 0, 0);
    return o;
}


void qc_reinit(void) {
    /* eski nesneler kabuk yeniden kurulumunda silinmis olabilir; zamanlayici
     * ekranda degil, o yuzden elle temizlenir */
    if (s_osd_t) { lv_timer_delete(s_osd_t); s_osd_t = 0; }
    s_qs = 0; s_nc = 0; s_nc_list = 0;
    s_osd = 0; s_osd_lbl = 0; s_osd_bar = 0;
    s_wifi_lbl = 0; s_air_lbl = 0; s_night_lbl = 0;
    s_ses_lbl = 0; s_par_lbl = 0; s_ses_sl = 0; s_par_sl = 0;

    lv_obj_t *scr = lv_screen_active();

    /* perdeler en alta (diğer her sey uzerine gelir) */
    s_dim = mk_perde(0x000000);
    s_night = mk_perde(0xFF9A3C);      /* amber — gece modu */
    lv_obj_move_background(s_dim);
    lv_obj_move_background(s_night);
    qc_apply_overlays();

    /* OSD */
    s_osd = lv_obj_create(scr);
    lv_obj_remove_style_all(s_osd);
    lv_obj_set_size(s_osd, 280, 60);
    lv_obj_set_style_radius(s_osd, 12, 0);
    lv_obj_set_style_bg_color(s_osd, lv_color_hex(ADL_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_osd, 245, 0);
    lv_obj_set_style_border_width(s_osd, 1, 0);
    lv_obj_set_style_border_color(s_osd, lv_color_hex(ADL_ACCENT), 0);
    lv_obj_set_style_shadow_width(s_osd, 16, 0);
    lv_obj_set_style_shadow_color(s_osd, lv_color_hex(0), 0);
    lv_obj_set_style_shadow_opa(s_osd, 130, 0);
    lv_obj_align(s_osd, LV_ALIGN_BOTTOM_MID, 0, -(TB_BH + TB_Y_OFF + 24));
    lv_obj_add_flag(s_osd, LV_OBJ_FLAG_HIDDEN);
    s_osd_lbl = lv_label_create(s_osd);
    lv_label_set_text(s_osd_lbl, "Ses %0");
    lv_obj_set_style_text_color(s_osd_lbl, lv_color_hex(ADL_TEXT), 0);
    lv_obj_set_pos(s_osd_lbl, 14, 10);
    s_osd_bar = lv_slider_create(s_osd);
    lv_obj_set_size(s_osd_bar, 252, 10);
    lv_obj_set_pos(s_osd_bar, 14, 36);
    lv_slider_set_range(s_osd_bar, 0, 100);
    lv_obj_clear_flag(s_osd_bar, LV_OBJ_FLAG_CLICKABLE);   /* yalnizca gosterge */

    qc_build_qs();
    qc_build_nc();
}


void qc_init(void) {
    qc_reinit();
}
