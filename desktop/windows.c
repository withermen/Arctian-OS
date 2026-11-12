#include "dsk.h"
#include "dpi.h"


/* ---------------------------------------------------------------- */
/* Uygulama bazinda pencere boyut/konum hafizasi (settings.cfg)      */
/* ---------------------------------------------------------------- */

static void win_geom_key(char *out, int cap, const char *title) {
    int n = 0;
    const char *p = "win_";
    while (*p && n < cap - 1) out[n++] = *p++;
    while (*title && n < cap - 1) out[n++] = *title++;
    out[n] = 0;
    for (int i = 0; out[i]; i++)
        if (out[i] == ' ' || out[i] == '/') out[i] = '_';
}


static void num_cat(char *out, int cap, int v, int sep) {
    int n = 0;
    while (out[n]) n++;
    char tmp[12];
    int tn = 0;
    if (v < 0) { if (n < cap - 1) out[n++] = '-'; v = -v; }
    if (v == 0) tmp[tn++] = '0';
    while (v && tn < 11) { tmp[tn++] = (char)('0' + v % 10); v /= 10; }
    while (tn && n < cap - 1) out[n++] = tmp[--tn];
    if (sep && n < cap - 1) out[n++] = ',';
    out[n] = 0;
}


static int win_geom_parse(const char *s, int *w, int *h, int *x, int *y) {
    int v[4];
    for (int k = 0; k < 4; k++) {
        int sign = 1, val = 0, any = 0;
        while (*s == ' ') s++;
        if (*s == '-') { sign = -1; s++; }
        while (*s >= '0' && *s <= '9') { val = val * 10 + (*s - '0'); s++; any = 1; }
        if (!any) return 0;
        v[k] = sign * val;
        if (*s == ',') s++;
    }
    if (v[0] < 260 || v[0] > 4096) return 0;
    if (v[1] < 180 || v[1] > 4096) return 0;
    if (v[2] < -1000 || v[2] > 8000) return 0;
    if (v[3] < -1000 || v[3] > 8000) return 0;
    *w = v[0]; *h = v[1]; *x = v[2]; *y = v[3];
    return 1;
}


static void win_geom_save(winrec_t *r) {
    if (!r || !r->win) return;
    /* tam ekran uygulama: geometri hatirlanmaz, her acilista tam ekran. */
    if (lv_obj_has_flag(r->win, LV_OBJ_FLAG_USER_3)) return;
    /* ekran kapli (buyuk) modda kapatildiysa buyutulmeden onceki geometri
     * hatirlanir (yoksa varsayilan boyutla yeniden acilir). */
    if (lv_obj_has_flag(r->win, LV_OBJ_FLAG_USER_1)) {
        if (r->max_saved_w > 0 && r->max_saved_h > 0) {
            char gk[32], gv[40];
            win_geom_key(gk, (int)sizeof(gk), r->title);
            gv[0] = 0;
            num_cat(gv, (int)sizeof(gv), r->max_saved_w, 1);
            num_cat(gv, (int)sizeof(gv), r->max_saved_h, 1);
            num_cat(gv, (int)sizeof(gv), r->max_saved_x, 1);
            num_cat(gv, (int)sizeof(gv), r->max_saved_y, 0);
            kv_set(gk, gv);
        }
        return;
    }
    char gk[32], gv[40];
    win_geom_key(gk, (int)sizeof(gk), r->title);
    gv[0] = 0;
    num_cat(gv, (int)sizeof(gv), lv_obj_get_width(r->win), 1);
    num_cat(gv, (int)sizeof(gv), lv_obj_get_height(r->win), 1);
    num_cat(gv, (int)sizeof(gv), lv_obj_get_x(r->win), 1);
    num_cat(gv, (int)sizeof(gv), lv_obj_get_y(r->win), 0);
    kv_set(gk, gv);
}


void update_app_indicators(void) {
    /* Taskbar kapsami degisti; ana dongude tb_poll yeniden kurar. */
    g_tb_dirty = 1;
}


/* Kapali pencerelerin kaydlari burada geri donusturulur; her acilista yeni
 * bellek istenmez, kayip da birikmez. */
static winrec_t *s_rec_free[MAX_WINS * 2];
static int s_rec_free_n;

void win_close_rec(winrec_t *r) {
    if (!r) return;
    win_geom_save(r);
    if (r->pid > 0 && g_asi->proc && g_asi->proc->app_close) {
        g_asi->proc->app_close(r->pid, 0);
        r->pid = 0;
    }
    if (r->btn) lv_obj_delete(r->btn);
    if (r->win) lv_obj_delete(r->win);
    r->btn = 0; r->win = 0;
    /* Slot temizlenir, kayit serbest listeye alinir. Eskiden ikisi de
     * yapilmiyordu: tablo 8 kayitla dolup tasiyor, kapanan pencerenin
     * kayiti kayboluyordu. Liste dolarsa kayit burada kalir (siztir) ama
     * bozuk durum olusmaz. */
    for (int i = 0; i < MAX_WINS; i++)
        if (g_recs[i] == r) { g_recs[i] = 0; break; }
    if (s_rec_free_n < (int)(sizeof(s_rec_free) / sizeof(s_rec_free[0])))
        s_rec_free[s_rec_free_n++] = r;
    update_app_indicators();
}


/* Acilis/kapanma solmasi: yalnizca opa oynatilir; boyut/konum degismez.
 * g_reduce_motion kapaliysa hic animasyon kurulmaz, opa aninda ayarlanir. */
static void win_fade_exec_cb(void * var, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

/* Solma bitti: mevcut silme yolunu cagrir. Pencere yol ustunde
 * shell_rebuild gibi bir yoldan erken silindiyse LVGL animasyonu da
 * siler ve bu callback calismaz; yine de kayit yeniden kullanilmis olabilecegi
 * icin winrec_alive + USER_2 (solma isareti) kontrol edilir. */
static void win_close_done_cb(lv_anim_t * a) {
    winrec_t * r = (winrec_t *)lv_anim_get_user_data(a);
    if (!r || !winrec_alive(r) || !r->win) return;
    if (!lv_obj_has_flag(r->win, LV_OBJ_FLAG_USER_2)) return;
    win_close_rec(r);
}

void win_close_cb(lv_event_t *e) {
    winrec_t * r = (winrec_t *)lv_event_get_user_data(e);
    if (!r || !r->win) return;
    if (g_reduce_motion) { win_close_rec(r); return; }
    /* cift cagri korumasi: solma suruyor, ikinci cagri no-op */
    if (lv_obj_has_flag(r->win, LV_OBJ_FLAG_USER_2)) return;
    lv_obj_add_flag(r->win, LV_OBJ_FLAG_USER_2);
    lv_anim_delete(r->win, win_fade_exec_cb);   /* ortada acilis solmasi varsa */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, r->win);
    lv_anim_set_values(&a, 255, 0);
    lv_anim_set_duration(&a, 110);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&a, win_fade_exec_cb);
    lv_anim_set_completed_cb(&a, win_close_done_cb);
    lv_anim_set_user_data(&a, r);
    lv_anim_start(&a);
}


void win_min_cb(lv_event_t *e) {
    winrec_t *r = (winrec_t *)lv_event_get_user_data(e);
    if (r && r->win) lv_obj_add_flag(r->win, LV_OBJ_FLAG_HIDDEN);
}


/* USER_2 isaretli govdeyi bulur (kabuk: launch.c koyar). Baslik cubugu
 * USER_2 tasimadigi icin bastan aramak yeterlidir. */
static lv_obj_t *win_body_of(lv_obj_t *win) {
    int n = lv_obj_get_child_cnt(win);
    for (int i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(win, i);
        if (c && lv_obj_has_flag(c, LV_OBJ_FLAG_USER_2)) return c;
    }
    return 0;
}

/* ---- Boyutlanima bildirimi yerlestirim disina tasinir ---------------
 * LV_EVENT_SIZE_CHANGED, lv_obj_set_size'in icinden — yani LVGL'nin
 * yerlestirim yiginin ortasinda — firlar. relayout callback'i govdedeki
 * yuzlerce nesneyi silip yeniden kurabiliyor (ofis uygulamalari); bunu
 * yerlestirmenin ortasinda kosmak LVGL ic yapilarini bozuyor (ofis calismasinda
 * Abakus'un #GP ile devrilmesinin kokü budur). Bildirimi tek atimlik
 * kisa bir zamanlayiciyla yerlestirim sonrasina akiyoruz; ust uste
 * boyut degisimleri tek kurulumda birlesir. Dosya yoneticisinin kendi
 * icinde kullandigi kalibin kabuk düzeyinde uygulanmis hali. */
static lv_timer_t *s_rel_timer;
static winrec_t   *s_rel_rec;
static lv_obj_t   *s_rel_win;

static void win_rel_fire(lv_timer_t *t) {
    (void)t;
    s_rel_timer = 0;                 /* repeat=1: LVGL zamanlayiciyi siler */
    winrec_t *r = s_rel_rec;
    lv_obj_t *win = s_rel_win;
    s_rel_rec = 0; s_rel_win = 0;
    /* Pencere bu sirada kapandiysa (veya kayit geri alindiyssa) dokunmayiz. */
    if (!r || !r->win || r->win != win || !r->relayout) return;
    lv_obj_t *body = win_body_of(win);
    r->relayout(body ? body : win);
}

static void win_rel_defer(winrec_t *r) {
    s_rel_rec = r;
    s_rel_win = r->win;
    if (s_rel_timer) {               /* bekleyen bildirim tazel: tek kurulum */
        lv_timer_reset(s_rel_timer);
        return;
    }
    s_rel_timer = lv_timer_create(win_rel_fire, 120, 0);
    lv_timer_set_repeat_count(s_rel_timer, 1);
}

/* pencere boyutu degince: baslik cubugu genislesin + icerik yeniden aksin.
 * launch.c'nin olusturdugu govde (USER2 isaretli) pencereyle birlikte
 * boyutlandirilir; uygulamalar icerigi govdeye cizdigi icin relayout da
 * govde nesnesini alir (pencereyi alsa baslik payi kadar kaydirirdi). */
static void win_size_cb(lv_event_t *e) {
    winrec_t *r = (winrec_t *)lv_event_get_user_data(e);
    if (!r || !r->win) return;
    int W = lv_obj_get_width(r->win);
    int H = lv_obj_get_height(r->win);
    /* tam ekran (USER_3): baslik cubugu yok; govde ekranin tamamini kaplar */
    if (lv_obj_has_flag(r->win, LV_OBJ_FLAG_USER_3)) {
        lv_obj_t *fsb = win_body_of(r->win);
        if (fsb) lv_obj_set_size(fsb, W, H);
        if (r->relayout) win_rel_defer(r);
        return;
    }
    lv_obj_t *tb = lv_obj_get_child(r->win, 0);
    int tbh = dpi_px(36);            /* baslik cubugu tabani 36 px, dpi olcekli */
    if (tb) lv_obj_set_size(tb, W - 2, tbh);
    lv_obj_t *body = win_body_of(r->win);
    if (body) lv_obj_set_size(body, W - 4, H - (tbh + 6));
    if (r->relayout) win_rel_defer(r);
}


/* Nesneyi SAHIP pencere kaydina baglar. Uygulamalar entry()'de kendilerine
 * verilen BODY nesnesini geri passer; dogru karsilastirma icin ata nesnelere
 * dogru cikilir (aksi halde kayit sessizce bulunamazdi). */
static winrec_t *winrec_own(lv_obj_t *o) {
    for (lv_obj_t *p = o; p; p = lv_obj_get_parent(p))
        for (int i = 0; i < MAX_WINS; i++)
            if (g_recs[i] && g_recs[i]->win == p) return g_recs[i];
    return 0;
}

/* Disariya acik sarmalayici: baska moduller (wlf_host) pencere kaydini
 * buradan alir; winrec_own dosya icinde kalmaya devam eder. */
winrec_t *winrec_of(lv_obj_t *o) { return winrec_own(o); }


void win_set_relayout(lv_obj_t *win, void (*cb)(lv_obj_t *)) {
    winrec_t *r = winrec_own(win);
    if (r) r->relayout = cb;
}


/* Pencereye ait klavye grubunu kaydeder. Odak yonetimi pencere one
 * geldiginde bu grubu geri yukler; boylece tuslar eski uygulamada kalmaz. */
void win_set_focus_group(lv_obj_t *win, lv_group_t *g) {
    winrec_t *r = winrec_own(win);
    if (r) r->fgroup = g;
}


/* ---------------- Sanal masaustleri ----------------
 * 3 calisma alani. Her pencere acildigi anda etkin alana atanir; alan
 * degisiminde yalnizca o alana ait pencereler gorunur. Simge durumuna
 * kucultme (kullanici) ile alan gizlemesi ayri tutulur (ws_hidden). */
int g_ws = 0;
#define WS_COUNT_MAX 4

/* Calisma alani sayisi ayardan gelir (0.6): 1..4, varsayilan 3. */
static int ws_count(void) {
    int n = kv_get_int("ws_count", 3);
    if (n < 1) n = 1;
    if (n > WS_COUNT_MAX) n = WS_COUNT_MAX;
    return n;
}

void workspace_apply(void) {
    for (int j = 0; j < MAX_WINS; j++) {
        winrec_t *r = g_recs[j];
        if (!r || !r->win) continue;
        if (r->ws == g_ws) {
            if (r->ws_hidden) {
                r->ws_hidden = 0;
                lv_obj_remove_flag(r->win, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            if (!lv_obj_has_flag(r->win, LV_OBJ_FLAG_HIDDEN)) {
                r->ws_hidden = 1;
                lv_obj_add_flag(r->win, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    /* yeni alanda one gelen pencere: odak da oraya tasinir */
    winrec_t *top = winrec_top();
    if (top && top->win) lv_obj_move_foreground(top->win);
    update_app_indicators();
}

/* Alan degisiminde kisa bir HUD gosterir (0.6): ust ortada "2 / 3".
 * Bildirim yigina da kayit dusulur. */
static void ws_hud_del_cb(lv_timer_t *t) {
    lv_obj_t *o = (lv_obj_t *)lv_timer_get_user_data(t);
    if (o) lv_obj_delete(o);
    lv_timer_delete(t);
}

void workspace_notify(void) {
    char b[8];
    b[0] = (char)('1' + g_ws);
    b[1] = 0;
    arctian_notify("Calisma alani", b);

    lv_obj_t *box = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(box);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(box, ui_s(220), ui_s(56));
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, ui_s(24));
    lv_obj_set_style_bg_color(box, lv_color_hex(ADL_SURFACE), 0);
    lv_obj_set_style_bg_opa(box, 230, 0);
    lv_obj_set_style_radius(box, ui_s(28), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(ADL_BORDER), 0);

    lv_obj_t *l = lv_label_create(box);
    char t[24];
    int k = 0;
    const char *pre = "Calisma alani ";
    for (int i = 0; pre[i] && k < (int)sizeof(t) - 4; i++) t[k++] = pre[i];
    t[k++] = (char)('1' + g_ws);
    t[k++] = ' ';
    t[k++] = '/';
    t[k++] = ' ';
    t[k++] = (char)('0' + ws_count());
    t[k] = 0;
    lv_label_set_text(l, t);
    lv_obj_set_style_text_color(l, lv_color_hex(ADL_TEXT), 0);
    lv_obj_set_style_text_font(l, adl_font(), 0);
    lv_obj_center(l);

    lv_obj_move_foreground(box);
    lv_timer_create(ws_hud_del_cb, 1200, box);
}


/* 0.6: etkin pencereyi baska calisma alanina tasir (Ctrl+Alt+Shift+,/.).
 * Pencere gizlenir ve alan degistiginde orada belirir. */
void workspace_move_top(int delta) {
    winrec_t *top = winrec_top();
    if (!top) return;
    int cnt = ws_count();
    int nw = top->ws + delta;
    if (nw < 0) nw = cnt - 1;
    if (nw >= cnt) nw = 0;
    if (nw == top->ws) return;
    top->ws = nw;

    /* Pencere su an gorunuyorsa alandan ayrildigi icin gizlenir. */
    if (nw != g_ws) {
        if (!lv_obj_has_flag(top->win, LV_OBJ_FLAG_HIDDEN)) {
            top->ws_hidden = 1;
            lv_obj_add_flag(top->win, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (top->ws_hidden) {
        top->ws_hidden = 0;
        lv_obj_remove_flag(top->win, LV_OBJ_FLAG_HIDDEN);
    }
    update_app_indicators();
    tb_ws_refresh();

    char b[8];
    b[0] = (char)('1' + nw);
    b[1] = 0;
    arctian_notify("Pencere tasindi", b);
}

void workspace_switch(int delta) {
    int cnt = ws_count();
    if (g_ws >= cnt) g_ws = cnt - 1;
    int nw = g_ws + delta;
    if (nw < 0) nw = cnt - 1;
    if (nw >= cnt) nw = 0;
    if (nw == g_ws) return;
    g_ws = nw;
    workspace_apply();
    workspace_notify();
    tb_ws_refresh();   /* gorev cubugundaki noktalar (0.6) */
}


/* Klavye kisayolunu pencereye yonlendir: masaustu bir textarea'ya
 * ulasamazsa bu cb cagrilir (Dosya Yoneticisi Ctrl+C/X/V/Z icin). */
void win_set_hotkey(lv_obj_t *win, void (*cb)(int hk)) {
    winrec_t *r = winrec_own(win);
    if (r) r->hotkey = cb;
}


/* Pencereyi surukle-birak hedefi yap.      */
void win_set_drop(lv_obj_t *win, int (*cb)(const char *src)) {
    winrec_t *r = winrec_own(win);
    if (r) r->drop = cb;
}


/* Uygulama API (app.h): pencere boyutu degisince icerigi yeniden yerlestir */
void arctian_shell_set_relayout(lv_obj_t *win, void (*cb)(lv_obj_t *)) {
    win_set_relayout(win, cb);
}


void arctian_shell_set_hotkey(lv_obj_t *win, void (*cb)(int hk)) {
    win_set_hotkey(win, cb);
}


void arctian_shell_set_drop(lv_obj_t *win, int (*cb)(const char *src)) {
    win_set_drop(win, cb);
}


/* (x,y) noktasindaki en ust pencereye dosya birak.      */
int arctian_shell_drop_at(int x, int y, const char *src_path) {
    winrec_t *r = win_at(x, y);
    return (r && r->drop && src_path) ? r->drop(src_path) : 0;
}


/* baslik cubuguna cift tik: ekran kapla / geri al */
void title_dbl_cb(lv_event_t *e) {
    win_max_cb(e);
}


void win_max_cb(lv_event_t *e) {
    winrec_t *r = (winrec_t *)lv_event_get_user_data(e);
    if (!r || !r->win) return;
    lv_obj_t *w = r->win;
    lv_obj_set_style_align(w, LV_ALIGN_DEFAULT, 0);
    if (lv_obj_has_flag(w, LV_OBJ_FLAG_USER_1)) {
        /* geri al: buyutulmeden onceki geometriye don.
         * kayit yoksa ortali varsayilan 520x340'a don. */
        if (r->max_saved_w > 0 && r->max_saved_h > 0) {
            lv_obj_set_size(w, r->max_saved_w, r->max_saved_h);
            lv_obj_set_pos(w, r->max_saved_x, r->max_saved_y);
        } else {
            /* ortali varsayilan: taban 520x340, dpi olcekli */
            int dw = dpi_px(520), dh = dpi_px(340);
            lv_obj_set_size(w, dw, dh);
            lv_obj_set_pos(w, (g_asi->gfx->width() - dw) / 2,
                           (g_asi->gfx->height() - dh) / 2);
        }
        lv_obj_remove_flag(w, LV_OBJ_FLAG_USER_1);
    } else {
        /* buyutmeden once mevcut geometriyi hatirla */
        r->max_saved_w = lv_obj_get_width(w);
        r->max_saved_h = lv_obj_get_height(w);
        r->max_saved_x = lv_obj_get_x(w);
        r->max_saved_y = lv_obj_get_y(w);
        /* kenar bosluklari taban 40/120 (genislik/yukseklik) ve 20/20,
         * hepsi dpi olcekli; ekran boyutu sabit degil, gfx'ten gelir */
        lv_obj_set_size(w, g_asi->gfx->width() - dpi_px(40),
                        g_asi->gfx->height() - dpi_px(120));
        lv_obj_set_pos(w, dpi_px(20), dpi_px(20));
        lv_obj_add_flag(w, LV_OBJ_FLAG_USER_1);
    }
    lv_obj_move_foreground(w);
}


/* baslik cubugundan surukleme (delta tabanli, guvenli) */
void title_drag_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *win = (lv_obj_t *)lv_event_get_user_data(e);
    if (!win) return;

    if (code == LV_EVENT_PRESSED) {
        lv_indev_t *indev = lv_indev_get_act();
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        g_last_x = p.x;
        g_last_y = p.y;
        lv_obj_set_style_align(win, LV_ALIGN_DEFAULT, 0);
        lv_obj_move_foreground(win);
    } else if (code == LV_EVENT_PRESSING) {
        lv_indev_t *indev = lv_indev_get_act();
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        int dx = p.x - g_last_x;
        int dy = p.y - g_last_y;
        g_last_x = p.x;
        g_last_y = p.y;

        int nx = lv_obj_get_x(win) + dx;
        int ny = lv_obj_get_y(win) + dy;

        /* pencereyi ekranda tut */
        int sw = g_asi->gfx->width();
        int sh = g_asi->gfx->height();
        int ww = lv_obj_get_width(win);
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if (nx + ww > sw) nx = sw - ww;
        if (ny > sh - dpi_px(120)) ny = sh - dpi_px(120);   /* altta gorunur kenar */
        lv_obj_set_pos(win, nx, ny);
    } else if (code == LV_EVENT_RELEASED) {
        int sw = g_asi->gfx->width();
        int sh = g_asi->gfx->height();
        int x = lv_obj_get_x(win);
        int y = lv_obj_get_y(win);
        int ww = lv_obj_get_width(win);
        int bottom = sh - (TB_BH + TB_Y_OFF);
        if (bottom < 200) bottom = sh;

        int snap = dpi_px(8);        /* kenere yapisma esigi de dpi olcekli */
        if (x <= snap) {
            lv_obj_set_pos(win, 0, 0);
            lv_obj_set_size(win, sw / 2, bottom);
        } else if (x + ww >= sw - snap) {
            lv_obj_set_pos(win, sw / 2, 0);
            lv_obj_set_size(win, sw - sw / 2, bottom);
        } else if (y <= snap) {
            lv_obj_set_pos(win, 0, 0);
            lv_obj_set_size(win, sw, bottom);
        }
    }
}


/* Spec: her dugme 46 px genislik, tam yukseklik, kare (yaricap yok);
 * uzerine gelince zemin dolar, kapat'in uzeri #c42b1c. */
void add_ctrl(lv_obj_t *tb, const char *svg, uint32_t bgcol,
                     lv_event_cb_t cb, void *ud, int xoff) {
    lv_obj_t *b = lv_button_create(tb);
    lv_obj_set_size(b, dpi_px(46), dpi_px(36));   /* taban 46x36, dpi olcekli */
    lv_obj_set_style_radius(b, 0, 0);
    /* Normalde seffaf; uzerine gelince renklenir (kapat = kirmizi). */
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bgcol), LV_STATE_HOVERED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_HOVERED);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, xoff, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *ic = make_icon(b, svg, dpi_px(12));
    lv_obj_center(ic);
}


lv_obj_t *create_window(const char *title, int w, int h) {
    int tbh = dpi_px(36);            /* baslik cubugu taban yuksekligi */
    int bwd = dpi_px(46);            /* kontrol dugmesi genisligi */
    int wx = (g_asi->gfx->width() - w) / 2;
    int wy = (g_asi->gfx->height() - h) / 2 - dpi_px(10);

    /* Uygulamada hatirlanan boyut/konum varsa uygula     */
    {
        char gk[32];
        win_geom_key(gk, (int)sizeof(gk), title);
        const char *gs = kv_get(gk, 0);
        int gw, gh, gx, gy;
        if (gs && win_geom_parse(gs, &gw, &gh, &gx, &gy)) {
            w = gw; h = gh;
            wx = gx; wy = gy;
            if (wx + w > g_asi->gfx->width()) wx = g_asi->gfx->width() - w;
            int mg = dpi_px(40);     /* geri yuklemede ust kenar payi */
            if (wy + h > g_asi->gfx->height() - mg) wy = g_asi->gfx->height() - mg - h;
            if (wx < 0) wx = 0;
            if (wy < 0) wy = 0;
        }
    }

    /* Ayarlar > Gorunum "roundwin": kapaliysa pencereler koseye yaklasan
     * yari capla cizilir (yeni acilan pencereler icin gecerlidir). */
    int wr = kv_get_int("roundwin", 1) ? 10 : 0;

    lv_obj_t *win = lv_obj_create(lv_screen_active());
    base_obj(win, ADL_SURFACE, wr, ADL_BORDER);
    lv_obj_set_style_bg_opa(win, 255, 0); /* spec: opak pencere zemini */
    lv_obj_set_style_pad_all(win, 0, 0);
    lv_obj_set_size(win, w, h);
    lv_obj_set_style_align(win, LV_ALIGN_DEFAULT, 0);
    lv_obj_set_pos(win, wx, wy);

    /* baslik cubugu (spec: pencereyle ayni zemin, altta 1 px ayirici) */
    lv_obj_t *tb = lv_obj_create(win);
    lv_obj_remove_style_all(tb);
    lv_obj_remove_flag(tb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(tb, lv_color_hex(ADL_SURFACE), 0);
    lv_obj_set_style_bg_opa(tb, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tb, wr, 0);
    lv_obj_set_style_border_width(tb, 1, 0);
    lv_obj_set_style_border_side(tb, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(tb, lv_color_hex(ADL_BORDER), 0);
    lv_obj_set_style_pad_left(tb, dpi_px(12), 0);
    lv_obj_set_size(tb, w - 2, tbh);
    lv_obj_align(tb, LV_ALIGN_TOP_MID, 0, 1);
    lv_obj_add_flag(tb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(tb, LV_OBJ_FLAG_PRESS_LOCK); /* imlec basliktan ciksa da basili kalsin */
    lv_obj_add_event_cb(tb, title_drag_cb, LV_EVENT_PRESSED, win);
    lv_obj_add_event_cb(tb, title_drag_cb, LV_EVENT_PRESSING, win);
    lv_obj_add_event_cb(tb, title_drag_cb, LV_EVENT_RELEASED, win);

    lv_obj_t *t = label(tb, title, 0xC9CED8);
    /* spec: baslik metni 12 px #c9ced8 (tema fontu 15 px'tir) */
    {
        extern const unsigned char font_ttf[];
        extern const unsigned int  font_ttf_len;
        static lv_font_t *s_title_font = 0;
        if (!s_title_font)
            s_title_font = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 12);
        if (s_title_font) lv_obj_set_style_text_font(t, s_title_font, 0);
    }
    lv_obj_align(t, LV_ALIGN_LEFT_MID, dpi_px(12), 0);

    /* running kaydi: once serbest listeden dener, yoksa yeni ayirir */
    winrec_t *r = 0;
    while (s_rec_free_n > 0 && !r) r = s_rec_free[--s_rec_free_n];
    if (!r) r = (winrec_t *)g_asi->mem->alloc(sizeof(winrec_t));
    if (r) {
    r->win = win; r->btn = 0;
    scopy(r->title, title, 39);
    r->pid = 0;
    r->relayout = 0;
    r->hotkey = 0; r->drop = 0;   /* geri donen kayıtta eski callback kalmasin */
    r->max_saved_w = r->max_saved_h = r->max_saved_x = r->max_saved_y = 0;
    r->fgroup = 0;
    r->ws = g_ws;
    r->ws_hidden = 0;
    if (g_asi->proc && g_asi->proc->app_open_caps)
        r->pid = g_asi->proc->app_open_caps(title, g_pending_caps);
    else if (g_asi->proc && g_asi->proc->app_open)
        r->pid = g_asi->proc->app_open(title);
    g_pending_caps = CAP_ALL;
    int slot = -1;
    for (int i = 0; i < MAX_WINS; i++) if (!g_recs[i]) { slot = i; break; }
    /* Yer yoksa mevcut kayit ezilmez; pencere kayitsiz calisir. */
    if (slot >= 0) g_recs[slot] = r;

    /* kontrol butonlari (kucult / buyut / kapat) - taban 46 px bloklar,
     * aralik da ayni olcekle kayar */
    add_ctrl(tb, SVG_CLOSE, 0xC42B1C, win_close_cb, r, 0);
    add_ctrl(tb, SVG_MAX,   0x262B35, win_max_cb,   r, -bwd);
    add_ctrl(tb, SVG_MIN,   0x262B35, win_min_cb,   r, -2 * bwd);

    lv_obj_add_event_cb(win, win_size_cb, LV_EVENT_SIZE_CHANGED, r);
    lv_obj_add_event_cb(tb, title_dbl_cb, LV_EVENT_DOUBLE_CLICKED, r);
    }
    /* acilis solmasi: opa 0 -> 255, ~140 ms ease-out; boyut/konum degmez */
    if (!g_reduce_motion) {
        lv_obj_set_style_opa(win, LV_OPA_TRANSP, 0);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, win);
        lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_duration(&a, 140);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&a, win_fade_exec_cb);
        lv_anim_start(&a);
    }
    update_app_indicators();
    lv_obj_move_foreground(win);
    return win;
}


/* 0.8: cercevesiz tam ekran pencere (ofis uygulamalari). Baslik cubugu,
 * dugme, yuvarlak köse YOK; uygulama kendi 36 px baslik cubugunu kendisi
 * cizer. Geometri settings.cfg'ye yazilmaz — her acilista tam ekrandir. */
lv_obj_t *create_window_fs(const char *title, int w, int h) {
    lv_obj_t *win = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(win);
    lv_obj_set_style_bg_color(win, lv_color_hex(ADL_SURFACE), 0);
    lv_obj_set_style_bg_opa(win, LV_OPA_COVER, 0);
    lv_obj_set_pos(win, 0, 0);
    lv_obj_set_size(win, w, h);
    lv_obj_remove_flag(win, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(win, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(win, LV_OBJ_FLAG_USER_3);   /* kabuk: bu tam ekrandir */

    winrec_t *r = 0;
    while (s_rec_free_n > 0 && !r) r = s_rec_free[--s_rec_free_n];
    if (!r) r = (winrec_t *)g_asi->mem->alloc(sizeof(winrec_t));
    if (r) {
        r->win = win; r->btn = 0;
        scopy(r->title, title, 39);
        r->pid = 0;
        r->relayout = 0;
        r->hotkey = 0; r->drop = 0;
        r->max_saved_w = r->max_saved_h = r->max_saved_x = r->max_saved_y = 0;
        r->fgroup = 0;
        r->ws = g_ws;
        r->ws_hidden = 0;
        if (g_asi->proc && g_asi->proc->app_open_caps)
            r->pid = g_asi->proc->app_open_caps(title, g_pending_caps);
        else if (g_asi->proc && g_asi->proc->app_open)
            r->pid = g_asi->proc->app_open(title);
        g_pending_caps = CAP_ALL;
        int slot = -1;
        for (int i = 0; i < MAX_WINS; i++) if (!g_recs[i]) { slot = i; break; }
        if (slot >= 0) g_recs[slot] = r;
        lv_obj_add_event_cb(win, win_size_cb, LV_EVENT_SIZE_CHANGED, r);
    }
    update_app_indicators();
    lv_obj_move_foreground(win);
    return win;
}

/* Ekranda gorunur tam ekran pencere var mi? Gorev cubugu buna gore
 * gizlenir (ofis uygulamalari acikken dock kaybolur). */
int win_fs_active(void) {
    for (int i = 0; i < MAX_WINS; i++) {
        winrec_t *r = g_recs[i];
        if (!r || !r->win) continue;
        if (!lv_obj_has_flag(r->win, LV_OBJ_FLAG_USER_3)) continue;
        if (lv_obj_has_flag(r->win, LV_OBJ_FLAG_HIDDEN)) continue;
        return 1;
    }
    return 0;
}


int win_pid(lv_obj_t *win) {
    for (int i = 0; i < MAX_WINS; i++)
        if (g_recs[i] && g_recs[i]->win == win) return g_recs[i]->pid;
    return 0;
}


winrec_t *winrec_top(void) {
    lv_obj_t *scr = lv_screen_active();
    uint32_t n = lv_obj_get_child_count(scr);
    for (int i = (int)n - 1; i >= 0; i--) {
        lv_obj_t *ch = lv_obj_get_child(scr, i);
        if (!ch || lv_obj_has_flag(ch, LV_OBJ_FLAG_HIDDEN)) continue;
        for (int j = 0; j < MAX_WINS; j++)
            if (g_recs[j] && g_recs[j]->win == ch) return g_recs[j];
    }
    return 0;
}


winrec_t *win_find(const char *title) {
    for (int j = 0; j < MAX_WINS; j++) {
        winrec_t *r = g_recs[j];
        if (r && r->win && title_eq(r->title, title)) return r;
    }
    return 0;
}

int winrec_alive(winrec_t *r) {
    if (!r) return 0;
    for (int i = 0; i < MAX_WINS; i++)
        if (g_recs[i] == r) return 1;
    return 0;
}

int window_at(int x, int y) {
    for (int i = 0; i < MAX_WINS; i++) {
        if (!g_recs[i] || !g_recs[i]->win) continue;
        if (rect_at(g_recs[i]->win, x, y)) return 1;
    }
    return 0;
}


/* Nokta uzerindeki (en ustteki) pencere kaydi — surukle-birak icin. */
winrec_t *win_at(int x, int y) {
    lv_obj_t *scr = lv_screen_active();
    uint32_t n = lv_obj_get_child_count(scr);
    for (int i = (int)n - 1; i >= 0; i--) {
        lv_obj_t *ch = lv_obj_get_child(scr, i);
        if (!ch || lv_obj_has_flag(ch, LV_OBJ_FLAG_HIDDEN)) continue;
        for (int j = 0; j < MAX_WINS; j++)
            if (g_recs[j] && g_recs[j]->win == ch && rect_at(ch, x, y))
                return g_recs[j];
    }
    return 0;
}

