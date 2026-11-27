#ifndef ARCTIAN_OOBE_H
#define ARCTIAN_OOBE_H

/* ============================================================================
 * Arctian OS 1.1 - Unified First-Run OOBE Wizard (4 Adim)
 *
 *  Adim 1 : Hos Geldiniz & Klavye Duzeni (TR-Q / TR-F / EN-Q)
 *  Adim 2 : Saat Dilimi & Tarih (UTC+3 Istanbul varsayilan, canli saat)
 *  Adim 3 : Gorunum & Kisisellestirme (Koyu / Acik tema + vurgu rengi)
 *  Adim 4 : Hizli Masaustu Turu ("Basla" -> oobe_done / setup_done / tour_done)
 * ============================================================================ */

static lv_obj_t   *s_oobe_root;
static lv_obj_t   *s_oobe_box;
static lv_obj_t   *s_oobe_hdr_title;
static lv_obj_t   *s_oobe_step_lbl;
static lv_obj_t   *s_oobe_dots[4];
static lv_obj_t   *s_oobe_body;
static lv_obj_t   *s_oobe_btn_back;
static lv_obj_t   *s_oobe_btn_back_lbl;
static lv_obj_t   *s_oobe_btn_next;
static lv_obj_t   *s_oobe_btn_next_lbl;
static lv_obj_t   *s_oobe_tz_clock_lbl;
static lv_group_t *s_oobe_group;
static lv_timer_t *s_oobe_timer;

static lv_font_t  *s_oobe_f_title;
static lv_font_t  *s_oobe_f_sub;
static lv_font_t  *s_oobe_f_small;

static int  s_oobe_step;
static int  s_oobe_gen;
static int  s_oobe_kbd;
static int  s_oobe_tz;
static int  s_oobe_light;
static int  s_oobe_init_light;

typedef struct {
    const char *label;
    int         minutes;
} oobe_tz_t;

static const oobe_tz_t s_oobe_tzs[] = {
    { "UTC+3  İstanbul",    180  },
    { "UTC+0  Londra",      0    },
    { "UTC+1  Berlin",      60   },
    { "UTC+2  Atina",       120  },
    { "UTC+4  Bakü",        240  },
    { "UTC-5  New York",    -300 },
    { "UTC-8  Los Angeles", -480 },
    { "UTC+9  Tokyo",       540  }
};

static const char *const s_oobe_months[12] = {
    "Ocak", "Şubat", "Mart", "Nisan", "Mayıs", "Haziran",
    "Temmuz", "Ağustos", "Eylül", "Ekim", "Kasım", "Aralık"
};

static const char *const s_oobe_wdays[7] = {
    "Pazar", "Pazartesi", "Salı", "Çarşamba", "Perşembe", "Cuma", "Cumartesi"
};

static void oobe_cat(char *dst, int cap, int *n, const char *s) {
    if (!dst || !s || !n || cap <= 0) return;
    for (int i = 0; s[i] && *n < cap - 1; i++) dst[(*n)++] = s[i];
    dst[*n] = 0;
}

static void oobe_cat_int(char *dst, int cap, int *n, int val, int pad2) {
    if (!dst || !n || cap <= 0) return;
    if (val < 0) {
        if (*n < cap - 1) dst[(*n)++] = '-';
        val = -val;
    }
    if (pad2 && val < 10 && *n < cap - 1) dst[(*n)++] = '0';
    char tmp[16];
    int k = 0;
    if (val == 0) tmp[k++] = '0';
    while (val > 0 && k < (int)sizeof(tmp)) {
        tmp[k++] = (char)('0' + (val % 10));
        val /= 10;
    }
    while (k > 0 && *n < cap - 1) dst[(*n)++] = tmp[--k];
    dst[*n] = 0;
}

static void oobe_sync_gen(void) {
    if (s_oobe_gen == g_shell_gen) return;
    s_oobe_root = 0;
    s_oobe_box = 0;
    s_oobe_hdr_title = 0;
    s_oobe_step_lbl = 0;
    for (int i = 0; i < 4; i++) s_oobe_dots[i] = 0;
    s_oobe_body = 0;
    s_oobe_btn_back = 0;
    s_oobe_btn_back_lbl = 0;
    s_oobe_btn_next = 0;
    s_oobe_btn_next_lbl = 0;
    s_oobe_tz_clock_lbl = 0;
    if (s_oobe_timer) { lv_timer_delete(s_oobe_timer); s_oobe_timer = 0; }
    s_oobe_gen = g_shell_gen;
}

static void oobe_render(void);

static void oobe_close(void) {
    oobe_sync_gen();
    if (s_oobe_timer) { lv_timer_delete(s_oobe_timer); s_oobe_timer = 0; }
    lv_indev_t *kp = lv_port_get_keypad();
    if (kp && s_oobe_group && lv_indev_get_group(kp) == s_oobe_group)
        lv_indev_set_group(kp, g_desktop_group);
    if (s_oobe_group) {
        lv_group_remove_all_objs(s_oobe_group);
        lv_group_delete(s_oobe_group);
        s_oobe_group = 0;
    }
    if (s_oobe_root) {
        lv_obj_delete(s_oobe_root);
        s_oobe_root = 0;
    }
    s_oobe_box = 0;
    s_oobe_hdr_title = 0;
    s_oobe_step_lbl = 0;
    for (int i = 0; i < 4; i++) s_oobe_dots[i] = 0;
    s_oobe_body = 0;
    s_oobe_btn_back = 0;
    s_oobe_btn_back_lbl = 0;
    s_oobe_btn_next = 0;
    s_oobe_btn_next_lbl = 0;
    s_oobe_tz_clock_lbl = 0;
}

static void oobe_finish(void) {
    int want_light = s_oobe_light;
    int init_light = s_oobe_init_light;
    kv_set("oobe_done", "1");
    kv_set("setup_done", "1");
    kv_set("tour_done", "1");
    oobe_close();
    if (want_light != init_light) {
        settings_set_int("light", want_light);
    }
}

static void oobe_format_now(char *out, int cap) {
    out[0] = 0;
    if (!g_asi || !g_asi->time || !g_asi->time->now) {
        int n = 0;
        oobe_cat(out, cap, &n, "Saat bilgisi bekleniyor...");
        return;
    }
    asi_datetime_t dt;
    if (!g_asi->time->now(&dt)) {
        int n = 0;
        oobe_cat(out, cap, &n, "Saat okunamadı");
        return;
    }
    int n = 0;
    const char *wd = (dt.weekday >= 0 && dt.weekday < 7) ? s_oobe_wdays[dt.weekday] : "";
    const char *mo = (dt.month >= 1 && dt.month <= 12) ? s_oobe_months[dt.month - 1] : "";
    oobe_cat_int(out, cap, &n, dt.day, 0);
    oobe_cat(out, cap, &n, " ");
    oobe_cat(out, cap, &n, mo);
    oobe_cat(out, cap, &n, " ");
    oobe_cat_int(out, cap, &n, dt.year, 0);
    if (wd[0]) {
        oobe_cat(out, cap, &n, " ");
        oobe_cat(out, cap, &n, wd);
    }
    oobe_cat(out, cap, &n, "   •   ");
    oobe_cat_int(out, cap, &n, dt.hour, 1);
    oobe_cat(out, cap, &n, ":");
    oobe_cat_int(out, cap, &n, dt.minute, 1);
    oobe_cat(out, cap, &n, ":");
    oobe_cat_int(out, cap, &n, dt.second, 1);
}

static void oobe_timer_cb(lv_timer_t *t) {
    (void)t;
    oobe_sync_gen();
    if (!s_oobe_root) return;
    lv_obj_move_foreground(s_oobe_root);
    lv_indev_t *kp = lv_port_get_keypad();
    if (kp && s_oobe_group && lv_indev_get_group(kp) != s_oobe_group)
        lv_indev_set_group(kp, s_oobe_group);
    if (s_oobe_step == 1 && s_oobe_tz_clock_lbl) {
        char buf[96];
        oobe_format_now(buf, sizeof(buf));
        lv_label_set_text(s_oobe_tz_clock_lbl, buf);
    }
}

static void oobe_kbd_cb(lv_event_t *e) {
    static const char *const codes[3] = { "trq", "trf", "enq" };
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx > 2) idx = 0;
    s_oobe_kbd = idx;
    if (g_asi && g_asi->input && g_asi->input->kbd_layout)
        g_asi->input->kbd_layout(idx);
    kv_set("kbd", codes[idx]);
    oobe_render();
}

static void oobe_tz_cb(lv_event_t *e) {
    int mins = (int)(intptr_t)lv_event_get_user_data(e);
    s_oobe_tz = mins;
    if (g_asi && g_asi->time && g_asi->time->set_tz_offset)
        g_asi->time->set_tz_offset(mins);
    char b[16];
    itoa_buf(mins, b, (int)sizeof(b));
    kv_set("tz_minutes", b);
    g_clock_last_sec = -1;
    clock_tick();
    oobe_render();
}

static void oobe_theme_cb(lv_event_t *e) {
    int light = (int)(intptr_t)lv_event_get_user_data(e) ? 1 : 0;
    s_oobe_light = light;
    kv_set("light", light ? "1" : "0");
    adl_set_light(light);
    adl_wolf_theme_init();
    oobe_render();
}

static void oobe_accent_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    settings_accent_set(idx);
    oobe_render();
}

static void oobe_back_cb(lv_event_t *e) {
    (void)e;
    if (s_oobe_step == 0) {
        oobe_finish();
        return;
    }
    s_oobe_step--;
    oobe_render();
}

static void oobe_next_cb(lv_event_t *e) {
    (void)e;
    if (s_oobe_step >= 3) {
        oobe_finish();
        return;
    }
    s_oobe_step++;
    oobe_render();
}

static void oobe_apply_chrome_colors(void) {
    if (s_oobe_root) {
        lv_obj_set_style_bg_color(s_oobe_root, lv_color_hex(ADL_BG), 0);
    }
    if (s_oobe_box) {
        lv_obj_set_style_bg_color(s_oobe_box, lv_color_hex(ADL_SURFACE), 0);
        lv_obj_set_style_border_color(s_oobe_box, lv_color_hex(ADL_BORDER), 0);
    }
    if (s_oobe_hdr_title) {
        lv_obj_set_style_text_color(s_oobe_hdr_title, lv_color_hex(ADL_TEXT), 0);
    }
    if (s_oobe_step_lbl) {
        lv_obj_set_style_text_color(s_oobe_step_lbl, lv_color_hex(ADL_ACCENT), 0);
        lv_label_set_text_fmt(s_oobe_step_lbl, "Adım %d / 4", s_oobe_step + 1);
    }
    for (int i = 0; i < 4; i++) {
        if (!s_oobe_dots[i]) continue;
        int active = (i <= s_oobe_step);
        lv_obj_set_style_bg_color(s_oobe_dots[i],
            lv_color_hex(active ? ADL_ACCENT : ADL_BORDER), 0);
    }
    if (s_oobe_btn_back && s_oobe_btn_back_lbl) {
        lv_obj_set_style_bg_color(s_oobe_btn_back, lv_color_hex(ADL_SURFACE_2), 0);
        lv_obj_set_style_border_color(s_oobe_btn_back, lv_color_hex(ADL_BORDER), 0);
        lv_obj_set_style_text_color(s_oobe_btn_back_lbl, lv_color_hex(ADL_TEXT), 0);
        lv_label_set_text(s_oobe_btn_back_lbl, (s_oobe_step == 0) ? "Atla" : "Geri");
    }
    if (s_oobe_btn_next && s_oobe_btn_next_lbl) {
        lv_obj_set_style_bg_color(s_oobe_btn_next, lv_color_hex(ADL_ACCENT), 0);
        lv_label_set_text(s_oobe_btn_next_lbl, (s_oobe_step >= 3) ? "Başla" : "İleri");
    }
}

static void oobe_build_step1(void) {
    const char *usr = arctian_auth_current();
    if (!usr || !usr[0]) usr = "Kullanıcı";

    char greet[96];
    int gn = 0;
    greet[0] = 0;
    oobe_cat(greet, sizeof(greet), &gn, "Hoş geldiniz, ");
    oobe_cat(greet, sizeof(greet), &gn, usr);
    oobe_cat(greet, sizeof(greet), &gn, "!");

    lv_obj_t *t = label(s_oobe_body, greet, ADL_TEXT);
    lv_obj_set_style_text_font(t, s_oobe_f_title ? s_oobe_f_title : adl_font(), 0);

    lv_obj_t *sub = label(s_oobe_body,
        "Arctian OS " ARCTIAN_VERSION " masaüstüne başlamadan önce klavye düzeninizi seçin.",
        ADL_TEXT_DIM);
    lv_obj_set_style_text_font(sub, s_oobe_f_small ? s_oobe_f_small : adl_font(), 0);
    lv_obj_set_width(sub, LV_PCT(100));
    lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);

    lv_obj_t *kl = label(s_oobe_body, "Klavye Düzeni", ADL_TEXT);
    lv_obj_set_style_text_font(kl, s_oobe_f_sub ? s_oobe_f_sub : adl_font(), 0);

    lv_obj_t *row = lv_obj_create(s_oobe_body);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, ui_s(96));
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, ui_s(14), 0);

    static const char *const k_code[3] = { "TR-Q", "TR-F", "EN-Q" };
    static const char *const k_desc[3] = { "Türkçe Q", "Türkçe F", "İngilizce Q" };

    for (int i = 0; i < 3; i++) {
        int sel = (s_oobe_kbd == i);
        lv_obj_t *b = lv_button_create(row);
        lv_obj_set_height(b, ui_s(90));
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, ui_s(12), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(ADL_SURFACE_2), 0);
        lv_obj_set_style_border_width(b, sel ? 2 : 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(sel ? ADL_ACCENT : ADL_BORDER), 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(b, ui_s(6), 0);
        lv_obj_add_event_cb(b, oobe_kbd_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *l1 = label(b, k_code[i], sel ? ADL_ACCENT : ADL_TEXT);
        lv_obj_set_style_text_font(l1, s_oobe_f_sub ? s_oobe_f_sub : adl_font(), 0);
        lv_obj_t *l2 = label(b, k_desc[i], ADL_TEXT_DIM);
        lv_obj_set_style_text_font(l2, s_oobe_f_small ? s_oobe_f_small : adl_font(), 0);
        if (s_oobe_group) lv_group_add_obj(s_oobe_group, b);
    }

    lv_obj_t *hint = label(s_oobe_body,
        "Klavye düzenini daha sonra Ayarlar > Klavye üzerinden dilediğiniz zaman değiştirebilirsiniz.",
        ADL_TEXT_DIM);
    lv_obj_set_style_text_font(hint, s_oobe_f_small ? s_oobe_f_small : adl_font(), 0);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
}

static void oobe_build_step2(void) {
    lv_obj_t *t = label(s_oobe_body, "Saat Dilimi ve Tarih", ADL_TEXT);
    lv_obj_set_style_text_font(t, s_oobe_f_title ? s_oobe_f_title : adl_font(), 0);

    lv_obj_t *sub = label(s_oobe_body,
        "Bulunduğunuz bölgeye göre saat dilimini seçin. Sistem saati anında güncellenir.",
        ADL_TEXT_DIM);
    lv_obj_set_style_text_font(sub, s_oobe_f_small ? s_oobe_f_small : adl_font(), 0);
    lv_obj_set_width(sub, LV_PCT(100));
    lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);

    lv_obj_t *banner = lv_obj_create(s_oobe_body);
    lv_obj_remove_style_all(banner);
    lv_obj_set_width(banner, LV_PCT(100));
    lv_obj_set_height(banner, ui_s(52));
    lv_obj_remove_flag(banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(banner, lv_color_hex(ADL_SURFACE_2), 0);
    lv_obj_set_style_bg_opa(banner, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(banner, ui_s(10), 0);
    lv_obj_set_style_border_width(banner, 1, 0);
    lv_obj_set_style_border_color(banner, lv_color_hex(ADL_ACCENT), 0);
    lv_obj_set_style_pad_hor(banner, ui_s(16), 0);

    char now_txt[96];
    oobe_format_now(now_txt, sizeof(now_txt));
    s_oobe_tz_clock_lbl = label(banner, now_txt, ADL_TEXT);
    lv_obj_set_style_text_font(s_oobe_tz_clock_lbl, s_oobe_f_sub ? s_oobe_f_sub : adl_font(), 0);
    lv_obj_center(s_oobe_tz_clock_lbl);

    int tz_cnt = (int)(sizeof(s_oobe_tzs) / sizeof(s_oobe_tzs[0]));
    for (int r = 0; r < 2; r++) {
        lv_obj_t *row = lv_obj_create(s_oobe_body);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, ui_s(56));
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, ui_s(10), 0);

        for (int c = 0; c < 4; c++) {
            int idx = r * 4 + c;
            if (idx >= tz_cnt) break;
            int sel = (s_oobe_tz == s_oobe_tzs[idx].minutes);
            lv_obj_t *b = lv_button_create(row);
            lv_obj_set_height(b, ui_s(52));
            lv_obj_set_flex_grow(b, 1);
            lv_obj_set_style_radius(b, ui_s(10), 0);
            lv_obj_set_style_bg_color(b, lv_color_hex(ADL_SURFACE_2), 0);
            lv_obj_set_style_border_width(b, sel ? 2 : 1, 0);
            lv_obj_set_style_border_color(b, lv_color_hex(sel ? ADL_ACCENT : ADL_BORDER), 0);
            lv_obj_set_style_shadow_width(b, 0, 0);
            lv_obj_add_event_cb(b, oobe_tz_cb, LV_EVENT_CLICKED,
                                (void *)(intptr_t)s_oobe_tzs[idx].minutes);

            lv_obj_t *bl = label(b, s_oobe_tzs[idx].label, sel ? ADL_ACCENT : ADL_TEXT);
            lv_obj_set_style_text_font(bl, s_oobe_f_small ? s_oobe_f_small : adl_font(), 0);
            lv_obj_center(bl);
            if (s_oobe_group) lv_group_add_obj(s_oobe_group, b);
        }
    }
}

static void oobe_build_step3(void) {
    lv_obj_t *t = label(s_oobe_body, "Görünüm ve Kişiselleştirme", ADL_TEXT);
    lv_obj_set_style_text_font(t, s_oobe_f_title ? s_oobe_f_title : adl_font(), 0);

    lv_obj_t *sub = label(s_oobe_body,
        "Tema kipini ve vurgu rengini seçin; tercihler anında uygulanır.",
        ADL_TEXT_DIM);
    lv_obj_set_style_text_font(sub, s_oobe_f_small ? s_oobe_f_small : adl_font(), 0);
    lv_obj_set_width(sub, LV_PCT(100));
    lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);

    lv_obj_t *trow = lv_obj_create(s_oobe_body);
    lv_obj_remove_style_all(trow);
    lv_obj_set_width(trow, LV_PCT(100));
    lv_obj_set_height(trow, ui_s(96));
    lv_obj_remove_flag(trow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(trow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(trow, ui_s(16), 0);

    static const char *const th_title[2] = { "Koyu Tema", "Açık Tema" };
    static const uint32_t    th_bg[2]    = { 0x0F1115,    0xEEF1F6 };
    static const uint32_t    th_fg[2]    = { 0xE8EAEE,    0x15181D };

    for (int i = 0; i < 2; i++) {
        int sel = (s_oobe_light == i);
        lv_obj_t *b = lv_button_create(trow);
        lv_obj_set_height(b, ui_s(90));
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, ui_s(12), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(th_bg[i]), 0);
        lv_obj_set_style_border_width(b, sel ? 2 : 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(sel ? ADL_ACCENT : ADL_BORDER), 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(b, ui_s(10), 0);
        lv_obj_add_event_cb(b, oobe_theme_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *bar = lv_obj_create(b);
        lv_obj_remove_style_all(bar);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(bar, ui_s(76), ui_s(8));
        lv_obj_set_style_radius(bar, ui_s(4), 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(ADL_ACCENT), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

        lv_obj_t *tl = label(b, th_title[i], th_fg[i]);
        lv_obj_set_style_text_font(tl, s_oobe_f_sub ? s_oobe_f_sub : adl_font(), 0);
        if (s_oobe_group) lv_group_add_obj(s_oobe_group, b);
    }

    int ac_cur = settings_accent_current();
    int ac_cnt = settings_accent_count();
    char ac_hdr[64];
    int an = 0;
    ac_hdr[0] = 0;
    oobe_cat(ac_hdr, sizeof(ac_hdr), &an, "Vurgu Rengi: ");
    oobe_cat(ac_hdr, sizeof(ac_hdr), &an, settings_accent_name(ac_cur));

    lv_obj_t *al = label(s_oobe_body, ac_hdr, ADL_TEXT);
    lv_obj_set_style_text_font(al, s_oobe_f_sub ? s_oobe_f_sub : adl_font(), 0);

    lv_obj_t *arow = lv_obj_create(s_oobe_body);
    lv_obj_remove_style_all(arow);
    lv_obj_set_width(arow, LV_PCT(100));
    lv_obj_set_height(arow, ui_s(48));
    lv_obj_remove_flag(arow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(arow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(arow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(arow, ui_s(12), 0);

    for (int i = 0; i < ac_cnt; i++) {
        int sel = (i == ac_cur);
        lv_obj_t *b = lv_button_create(arow);
        lv_obj_set_size(b, ui_s(38), ui_s(38));
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(b, sel ? 2 : 0, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(ADL_TEXT), 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_add_event_cb(b, oobe_accent_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *dot = lv_obj_create(b);
        lv_obj_remove_style_all(dot);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(dot, ui_s(30), ui_s(30));
        lv_obj_center(dot);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(settings_accent_color(i)), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        if (s_oobe_group) lv_group_add_obj(s_oobe_group, b);
    }
}

static void oobe_build_step4(void) {
    lv_obj_t *t = label(s_oobe_body, "Hızlı Masaüstü Turu", ADL_TEXT);
    lv_obj_set_style_text_font(t, s_oobe_f_title ? s_oobe_f_title : adl_font(), 0);

    lv_obj_t *sub = label(s_oobe_body,
        "Arctian OS " ARCTIAN_VERSION " masaüstü ortamı hazır. İşte temel ipuçları:",
        ADL_TEXT_DIM);
    lv_obj_set_style_text_font(sub, s_oobe_f_small ? s_oobe_f_small : adl_font(), 0);
    lv_obj_set_width(sub, LV_PCT(100));
    lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);

    static const char *const titles[4] = {
        "Masaüstüne Hoş Geldiniz",
        "Başlat Menüsü",
        "Her Şey Ayarlar'da",
        "Dosyalarınız ve Kısayollar"
    };
    static const char *const bodies[4] = {
        "Simgelere çift tıklayarak uygulama açın. Alt+Tab pencereler arasında hızlı geçiş yapar.",
        "Soldaki kurt simgesi tüm uygulamaları listeler; arama kutusu hepsini anında bulur.",
        "Ekran, ağ, ses, güncelleme ve sistem araçları tek yerde toplandı.",
        "C: ve D: (USB) sürücülerini gezin. Ctrl+Alt+T terminal, Ctrl+Alt+H yardım açar."
    };

    for (int r = 0; r < 2; r++) {
        lv_obj_t *row = lv_obj_create(s_oobe_body);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, ui_s(92));
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, ui_s(12), 0);

        for (int c = 0; c < 2; c++) {
            int idx = r * 2 + c;
            lv_obj_t *card = lv_obj_create(row);
            lv_obj_remove_style_all(card);
            lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_height(card, ui_s(88));
            lv_obj_set_flex_grow(card, 1);
            lv_obj_set_style_radius(card, ui_s(10), 0);
            lv_obj_set_style_bg_color(card, lv_color_hex(ADL_SURFACE_2), 0);
            lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(card, 1, 0);
            lv_obj_set_style_border_color(card, lv_color_hex(ADL_BORDER), 0);
            lv_obj_set_style_pad_all(card, ui_s(12), 0);
            lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_row(card, ui_s(4), 0);

            lv_obj_t *ct = label(card, titles[idx], ADL_ACCENT);
            lv_obj_set_style_text_font(ct, s_oobe_f_small ? s_oobe_f_small : adl_font(), 0);

            lv_obj_t *cb = label(card, bodies[idx], ADL_TEXT_DIM);
            lv_obj_set_style_text_font(cb, s_oobe_f_small ? s_oobe_f_small : adl_font(), 0);
            lv_obj_set_width(cb, LV_PCT(100));
            lv_label_set_long_mode(cb, LV_LABEL_LONG_WRAP);
        }
    }
}

static void oobe_render(void) {
    oobe_sync_gen();
    if (!s_oobe_root || !s_oobe_body) return;

    s_oobe_tz_clock_lbl = 0;
    if (s_oobe_group) lv_group_remove_all_objs(s_oobe_group);
    lv_obj_clean(s_oobe_body);

    oobe_apply_chrome_colors();

    switch (s_oobe_step) {
        case 0:  oobe_build_step1(); break;
        case 1:  oobe_build_step2(); break;
        case 2:  oobe_build_step3(); break;
        default: oobe_build_step4(); break;
    }

    if (s_oobe_group) {
        if (s_oobe_btn_back) lv_group_add_obj(s_oobe_group, s_oobe_btn_back);
        if (s_oobe_btn_next) lv_group_add_obj(s_oobe_group, s_oobe_btn_next);
        if (s_oobe_btn_next)
            lv_group_focus_obj(s_oobe_btn_next);
    }

    lv_indev_t *kp = lv_port_get_keypad();
    if (kp && s_oobe_group) lv_indev_set_group(kp, s_oobe_group);
    lv_obj_move_foreground(s_oobe_root);
}

static inline void oobe_show(void) {
    oobe_sync_gen();
    if (s_oobe_root) return;

    if (!s_oobe_f_title) s_oobe_f_title = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 22);
    if (!s_oobe_f_sub)   s_oobe_f_sub   = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 15);
    if (!s_oobe_f_small) s_oobe_f_small = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 13);

    {
        const char *kbd = kv_get("kbd", "trq");
        s_oobe_kbd = (kbd && kbd[0] == 'e') ? 2 : ((kbd && kbd[2] == 'f') ? 1 : 0);
    }
    s_oobe_tz = kv_get_int("tz_minutes", 180);
    s_oobe_light = adl_light() ? 1 : 0;
    s_oobe_init_light = s_oobe_light;
    s_oobe_step = 0;

    int sw = (g_asi && g_asi->gfx && g_asi->gfx->width) ? g_asi->gfx->width() : 1280;
    int sh = (g_asi && g_asi->gfx && g_asi->gfx->height) ? g_asi->gfx->height() : 720;
    int w = ui_s(880);
    int h = ui_s(560);
    if (w > sw - 48) w = sw - 48;
    if (h > sh - 48) h = sh - 48;

    s_oobe_root = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_oobe_root);
    lv_obj_set_size(s_oobe_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_oobe_root, lv_color_hex(ADL_BG), 0);
    lv_obj_set_style_bg_opa(s_oobe_root, 232, 0);
    lv_obj_add_flag(s_oobe_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(s_oobe_root);

    s_oobe_box = lv_obj_create(s_oobe_root);
    lv_obj_remove_style_all(s_oobe_box);
    lv_obj_remove_flag(s_oobe_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_oobe_box, w, h);
    lv_obj_center(s_oobe_box);
    lv_obj_set_style_radius(s_oobe_box, ui_s(16), 0);
    lv_obj_set_style_bg_color(s_oobe_box, lv_color_hex(ADL_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_oobe_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_oobe_box, 1, 0);
    lv_obj_set_style_border_color(s_oobe_box, lv_color_hex(ADL_BORDER), 0);
    lv_obj_set_style_shadow_width(s_oobe_box, ui_s(28), 0);
    lv_obj_set_style_shadow_color(s_oobe_box, lv_color_hex(0), 0);
    lv_obj_set_style_shadow_opa(s_oobe_box, 170, 0);
    lv_obj_set_style_pad_all(s_oobe_box, ui_s(26), 0);
    lv_obj_set_style_pad_row(s_oobe_box, ui_s(14), 0);
    lv_obj_set_flex_flow(s_oobe_box, LV_FLEX_FLOW_COLUMN);

    /* Ust baslik + adim gostergesi */
    lv_obj_t *hdr = lv_obj_create(s_oobe_box);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_width(hdr, LV_PCT(100));
    lv_obj_set_height(hdr, ui_s(42));
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *logo = make_logo(hdr, ui_s(34));
    lv_obj_align(logo, LV_ALIGN_LEFT_MID, 0, 0);

    s_oobe_hdr_title = label(hdr, "Arctian OS Kurulum Sihirbazı", ADL_TEXT);
    lv_obj_set_style_text_font(s_oobe_hdr_title, s_oobe_f_sub ? s_oobe_f_sub : adl_font(), 0);
    lv_obj_align(s_oobe_hdr_title, LV_ALIGN_LEFT_MID, ui_s(46), 0);

    s_oobe_step_lbl = label(hdr, "Adım 1 / 4", ADL_ACCENT);
    lv_obj_set_style_text_font(s_oobe_step_lbl, s_oobe_f_small ? s_oobe_f_small : adl_font(), 0);
    lv_obj_align(s_oobe_step_lbl, LV_ALIGN_RIGHT_MID, -ui_s(110), 0);

    for (int i = 0; i < 4; i++) {
        lv_obj_t *dot = lv_obj_create(hdr);
        lv_obj_remove_style_all(dot);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(dot, ui_s(20), ui_s(6));
        lv_obj_set_style_radius(dot, ui_s(3), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_align(dot, LV_ALIGN_RIGHT_MID, -ui_s((3 - i) * 24), 0);
        s_oobe_dots[i] = dot;
    }

    /* Adim icerik alani */
    s_oobe_body = lv_obj_create(s_oobe_box);
    lv_obj_remove_style_all(s_oobe_body);
    lv_obj_set_width(s_oobe_body, LV_PCT(100));
    lv_obj_set_flex_grow(s_oobe_body, 1);
    lv_obj_remove_flag(s_oobe_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_oobe_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_oobe_body, ui_s(12), 0);

    /* Alt gezinme cubugu */
    lv_obj_t *ftr = lv_obj_create(s_oobe_box);
    lv_obj_remove_style_all(ftr);
    lv_obj_set_width(ftr, LV_PCT(100));
    lv_obj_set_height(ftr, ui_s(48));
    lv_obj_remove_flag(ftr, LV_OBJ_FLAG_SCROLLABLE);

    s_oobe_btn_back = lv_button_create(ftr);
    lv_obj_set_size(s_oobe_btn_back, ui_s(124), ui_s(42));
    lv_obj_align(s_oobe_btn_back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(s_oobe_btn_back, ui_s(10), 0);
    lv_obj_set_style_border_width(s_oobe_btn_back, 1, 0);
    lv_obj_set_style_shadow_width(s_oobe_btn_back, 0, 0);
    s_oobe_btn_back_lbl = label(s_oobe_btn_back, "Atla", ADL_TEXT);
    lv_obj_set_style_text_font(s_oobe_btn_back_lbl, s_oobe_f_sub ? s_oobe_f_sub : adl_font(), 0);
    lv_obj_center(s_oobe_btn_back_lbl);
    lv_obj_add_event_cb(s_oobe_btn_back, oobe_back_cb, LV_EVENT_CLICKED, 0);

    s_oobe_btn_next = lv_button_create(ftr);
    lv_obj_set_size(s_oobe_btn_next, ui_s(144), ui_s(42));
    lv_obj_align(s_oobe_btn_next, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(s_oobe_btn_next, ui_s(10), 0);
    lv_obj_set_style_border_width(s_oobe_btn_next, 0, 0);
    lv_obj_set_style_shadow_width(s_oobe_btn_next, 0, 0);
    s_oobe_btn_next_lbl = label(s_oobe_btn_next, "İleri", 0xFFFFFF);
    lv_obj_set_style_text_font(s_oobe_btn_next_lbl, s_oobe_f_sub ? s_oobe_f_sub : adl_font(), 0);
    lv_obj_center(s_oobe_btn_next_lbl);
    lv_obj_add_event_cb(s_oobe_btn_next, oobe_next_cb, LV_EVENT_CLICKED, 0);

    if (!s_oobe_group) s_oobe_group = lv_group_create();
    if (!s_oobe_timer) s_oobe_timer = lv_timer_create(oobe_timer_cb, 250, 0);

    oobe_render();
}

#endif /* ARCTIAN_OOBE_H */
