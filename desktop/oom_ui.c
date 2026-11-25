/* ============================================================================
 * Arctian OS - Power-Menu-Style "RAM Doldu!" OOM Recovery Overlay
 *
 * Bellek kritik seviyeye ulastiginda (g_asi->proc->oom_state() >= 2) veya
 * test/tani kisayolu (Ctrl+Shift+M) ile acilir:
 *   1. Calisan kullanici/WLF/POSIX gorevlerini g_asi->proc->oom_freeze_apps()
 *      ile guvenle dondurur (masaustu compositor + girdi + cekirdek ayakta kalir).
 *   2. Cekirdek/WLF/POSIX sureclerini ve acik masaustu pencerelerini birlestirip
 *      tekillestirerek RAM kullanimina gore siralar.
 *   3. Kullanicinin istedigi uygulamalari [Sonlandir] ile kapatmasina veya
 *      [Sistemi Yeniden Baslat] ile sistemi yeniden baslatmasina izin verir.
 *   4. Bellek kritik esigin altina indiginde [Masaustune Don] ile
 *      oom_thaw_apps() cagirarak masaustune puruzsuz geri doner.
 * ============================================================================ */
#include "dsk.h"
#include "oom_ui.h"
#include <stdint.h>

#define OOM_KIND_DESKTOP 3
#define OOM_UI_MAX_ITEMS 128

typedef struct {
    int       pid;
    uint32_t  ram_kb;
    uint8_t   kind;     /* ASI_OOM_KIND_* veya OOM_KIND_DESKTOP */
    uint8_t   frozen;
    winrec_t *rec;
    char      name[48];
} oom_item_t;

static const char SVG_OOM_WARN[] =
    "<svg viewBox=\"0 0 36 36\">"
    "<path d=\"M18 4 L33 30 L3 30 Z\" fill=\"#F59E0B\" fill-opacity=\"0.18\" "
    "stroke=\"#F59E0B\" stroke-width=\"2.2\" stroke-linejoin=\"round\"/>"
    "<line x1=\"18\" y1=\"13\" x2=\"18\" y2=\"21\" stroke=\"#FBBF24\" "
    "stroke-width=\"2.6\" stroke-linecap=\"round\"/>"
    "<circle cx=\"18\" cy=\"25.5\" r=\"1.6\" fill=\"#FBBF24\"/>"
    "</svg>";

static lv_obj_t   *s_ov = 0;
static lv_obj_t   *s_panel = 0;
static lv_obj_t   *s_ram_text = 0;
static lv_obj_t   *s_ram_badge = 0;
static lv_obj_t   *s_ram_badge_lbl = 0;
static lv_obj_t   *s_ram_fill = 0;
static lv_obj_t   *s_ram_track = 0;
static lv_obj_t   *s_sec_lbl = 0;
static lv_obj_t   *s_list = 0;
static lv_obj_t   *s_status_lbl = 0;
static lv_obj_t   *s_btn_resume = 0;
static lv_obj_t   *s_btn_reboot = 0;
static lv_group_t *s_oom_group = 0;

static lv_font_t  *s_f_title = 0;
static lv_font_t  *s_f_sub = 0;
static lv_font_t  *s_f_small = 0;

static int         s_open = 0;
static int         s_manual = 0;
static int         s_sel = 0;
static int         s_killed_any = 0;
static int         s_pending_kill = -1;
static int         s_pending_exit = 0;
static uint64_t    s_last_refresh_ms = 0;
static uint64_t    s_last_hotkey_ms = 0;

static oom_item_t  s_items[OOM_UI_MAX_ITEMS];
static lv_obj_t   *s_row_btns[OOM_UI_MAX_ITEMS];
static int         s_item_n = 0;

/* ---- Kucuk formatlama ve seri gunluk yardimcilari ---- */

static void oom_log(const char *s) {
    if (s && g_asi && g_asi->console && g_asi->console->puts)
        g_asi->console->puts(s);
}

static void oom_buf_cat(char *dst, int cap, int *n, const char *s) {
    if (!dst || !s || !n || cap <= 0) return;
    for (int i = 0; s[i] && *n < cap - 1; i++)
        dst[(*n)++] = s[i];
    dst[*n] = 0;
}

static void oom_buf_u64(char *dst, int cap, int *n, uint64_t v) {
    char tmp[24];
    int k = 0;
    if (v == 0) tmp[k++] = '0';
    while (v > 0 && k < (int)sizeof(tmp)) {
        tmp[k++] = (char)('0' + (int)(v % 10ull));
        v /= 10ull;
    }
    while (k > 0 && *n < cap - 1)
        dst[(*n)++] = tmp[--k];
    dst[*n] = 0;
}

static void oom_fmt_ram(char *out, int cap, uint32_t kb) {
    int n = 0;
    out[0] = 0;
    if (kb >= 1024u) {
        uint32_t mb10 = (kb * 10u + 512u) / 1024u;
        uint32_t whole = mb10 / 10u;
        uint32_t frac = mb10 % 10u;
        oom_buf_u64(out, cap, &n, whole);
        if (whole < 100u && frac > 0u) {
            oom_buf_cat(out, cap, &n, ".");
            oom_buf_u64(out, cap, &n, frac);
        }
        oom_buf_cat(out, cap, &n, " MB");
    } else {
        oom_buf_u64(out, cap, &n, kb);
        oom_buf_cat(out, cap, &n, " KB");
    }
}

static const char *oom_kind_label(uint8_t kind) {
    switch (kind) {
        case ASI_OOM_KIND_POSIX: return "POSIX";
        case ASI_OOM_KIND_WLF:   return "WLF";
        case OOM_KIND_DESKTOP:   return "Masaustu";
        default:                 return "Sistem";
    }
}

static uint32_t oom_kind_color(uint8_t kind) {
    switch (kind) {
        case ASI_OOM_KIND_POSIX: return 0x164E63;
        case ASI_OOM_KIND_WLF:   return 0x3B2259;
        case OOM_KIND_DESKTOP:   return 0x1E3A5F;
        default:                 return 0x27272A;
    }
}

static uint32_t oom_kind_border(uint8_t kind) {
    switch (kind) {
        case ASI_OOM_KIND_POSIX: return 0x0EA5E9;
        case ASI_OOM_KIND_WLF:   return 0xA855F7;
        case OOM_KIND_DESKTOP:   return 0x3B82F6;
        default:                 return 0x52525B;
    }
}

static uint32_t oom_est_win_kb(winrec_t *r) {
    if (!r || !r->win) return 512u;
    int pkb = posix_win_ram_kb(r->win);
    if (pkb > 0) return (uint32_t)pkb;
    int w = lv_obj_get_width(r->win);
    int h = lv_obj_get_height(r->win);
    if (w < 100) w = 520;
    if (h < 100) h = 340;
    uint32_t pix_kb = ((uint32_t)w * (uint32_t)h * 4u) / 1024u;
    return pix_kb + 512u;
}

static uint8_t oom_classify_win(winrec_t *r, uint8_t kernel_kind) {
    if (!r || !r->win) return kernel_kind;
    if (posix_win_ram_kb(r->win) > 0) return ASI_OOM_KIND_POSIX;
    int ai = app_idx_by_title(r->title);
    if (ai >= arctian_app_count) return ASI_OOM_KIND_WLF;
    return OOM_KIND_DESKTOP;
}

/* ---- Birlesik uygulama listesi olusturma & tekillestirme ---- */

static void oom_collect_items(void) {
    s_item_n = 0;

    asi_oom_app_t kapps[64];
    int kcnt = 0;
    if (g_asi && g_asi->proc && g_asi->proc->oom_list_apps)
        kcnt = g_asi->proc->oom_list_apps(kapps, 64);

    uint8_t win_linked[MAX_WINS];
    for (int i = 0; i < MAX_WINS; i++) win_linked[i] = 0;

    /* 1) Cekirdek / WLF / POSIX surecleri; ayni PID'li masaustu penceresiyle birlestir */
    for (int i = 0; i < kcnt && s_item_n < OOM_UI_MAX_ITEMS; i++) {
        if (kapps[i].pid <= 1) continue;

        winrec_t *linked_rec = 0;
        for (int j = 0; j < MAX_WINS; j++) {
            winrec_t *r = g_recs[j];
            if (!r || !r->win || win_linked[j]) continue;
            if (r->pid == kapps[i].pid) {
                linked_rec = r;
                win_linked[j] = 1;
                break;
            }
        }
        /* POSIX sureci veya baslik eslesmesi: eger PID henuz baglanmadiysa ada gore tekillestir */
        if (!linked_rec && kapps[i].kind == ASI_OOM_KIND_POSIX) {
            for (int j = 0; j < MAX_WINS; j++) {
                winrec_t *r = g_recs[j];
                if (!r || !r->win || win_linked[j]) continue;
                if (posix_win_ram_kb(r->win) > 0 && title_eq(r->title, kapps[i].name)) {
                    linked_rec = r;
                    win_linked[j] = 1;
                    break;
                }
            }
        }

        oom_item_t *it = &s_items[s_item_n++];
        it->pid = kapps[i].pid;
        it->rec = linked_rec;
        it->frozen = kapps[i].frozen ? 1u : (linked_rec ? 1u : 0u);
        it->kind = linked_rec ? oom_classify_win(linked_rec, kapps[i].kind) : kapps[i].kind;
        it->ram_kb = kapps[i].ram_kb;
        if (linked_rec) {
            uint32_t est = oom_est_win_kb(linked_rec);
            if (it->ram_kb <= 64u || it->kind == ASI_OOM_KIND_POSIX)
                it->ram_kb = (est > it->ram_kb) ? est : it->ram_kb;
            scopy(it->name, linked_rec->title[0] ? linked_rec->title : kapps[i].name, 47);
        } else {
            scopy(it->name, kapps[i].name[0] ? kapps[i].name : "Gorev", 47);
        }
    }

    /* 2) Henuz listelenmemis acik masaustu / POSIX pencereleri */
    for (int j = 0; j < MAX_WINS && s_item_n < OOM_UI_MAX_ITEMS; j++) {
        winrec_t *r = g_recs[j];
        if (!r || !r->win || win_linked[j]) continue;

        oom_item_t *it = &s_items[s_item_n++];
        it->pid = (r->pid > 1) ? r->pid : 0;
        it->rec = r;
        it->frozen = 1u;
        it->kind = oom_classify_win(r, OOM_KIND_DESKTOP);
        it->ram_kb = oom_est_win_kb(r);
        scopy(it->name, r->title[0] ? r->title : "Pencere", 47);
    }

    /* 3) En cok RAM tuketen uygulamalar ustte olacak sekilde sirala */
    for (int i = 0; i < s_item_n - 1; i++) {
        for (int j = i + 1; j < s_item_n; j++) {
            int swap = 0;
            int prio_i = (s_items[i].kind != ASI_OOM_KIND_NATIVE) ? 1 : 0;
            int prio_j = (s_items[j].kind != ASI_OOM_KIND_NATIVE) ? 1 : 0;
            if (prio_j > prio_i) swap = 1;
            else if (prio_j == prio_i && s_items[j].ram_kb > s_items[i].ram_kb) swap = 1;
            if (swap) {
                oom_item_t tmp = s_items[i];
                s_items[i] = s_items[j];
                s_items[j] = tmp;
            }
        }
    }

    if (s_sel >= s_item_n) s_sel = s_item_n - 1;
    if (s_sel < 0) s_sel = 0;
}

static void oom_rebuild_list(void);
static void oom_refresh_ram_bar(void);

static void oom_kill_index(int idx) {
    if (idx < 0 || idx >= s_item_n) return;
    oom_item_t it = s_items[idx];

    char msg[128];
    int mn = 0;
    msg[0] = 0;
    oom_buf_cat(msg, sizeof(msg), &mn, "[oom-ui] terminating app '");
    oom_buf_cat(msg, sizeof(msg), &mn, it.name);
    oom_buf_cat(msg, sizeof(msg), &mn, "' pid=");
    oom_buf_u64(msg, sizeof(msg), &mn, (uint64_t)(it.pid > 0 ? it.pid : 0));
    oom_buf_cat(msg, sizeof(msg), &mn, " kind=");
    oom_buf_cat(msg, sizeof(msg), &mn, oom_kind_label(it.kind));
    oom_buf_cat(msg, sizeof(msg), &mn, "\n");
    oom_log(msg);

    if (it.pid > 1 && g_asi && g_asi->proc && g_asi->proc->oom_kill_app) {
        g_asi->proc->oom_kill_app(it.pid);
    }

    if (it.rec && winrec_alive(it.rec)) {
        /* pid zaten oom_kill_app ile sonlandirildiysa cift app_close onlenir */
        if (it.pid > 1) it.rec->pid = 0;
        win_close_rec(it.rec);
    } else if (it.pid > 1) {
        /* Eger baska bir winrec bu PID'ye bagliysa temizle */
        for (int j = 0; j < MAX_WINS; j++) {
            winrec_t *r = g_recs[j];
            if (r && r->win && r->pid == it.pid) {
                r->pid = 0;
                win_close_rec(r);
                break;
            }
        }
    }

    s_killed_any = 1;
    if (s_status_lbl) {
        char st[96];
        int sn = 0;
        st[0] = 0;
        oom_buf_cat(st, sizeof(st), &sn, "Sonlandirildi: ");
        oom_buf_cat(st, sizeof(st), &sn, it.name);
        lv_label_set_text(s_status_lbl, st);
        lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0x4ADE80), 0);
    }

    oom_collect_items();
    oom_refresh_ram_bar();
    oom_rebuild_list();
    oom_log("[oom-ui] list refreshed\n");
}

/* ---- Arayuz guncelleme & olay isleyiciler ---- */

static int oom_can_resume(void) {
    if (!g_asi || !g_asi->proc || !g_asi->proc->oom_state) return 1;
    return g_asi->proc->oom_state() < ASI_OOM_CRITICAL;
}

static void oom_refresh_ram_bar(void) {
    if (!s_open) return;

    int oom_lvl = (g_asi && g_asi->proc && g_asi->proc->oom_state)
                  ? g_asi->proc->oom_state() : ASI_OOM_OK;

    asi_meminfo_t mi;
    mi.total = 0; mi.used = 0; mi.free = 0;
    mi.heap_total = 0; mi.heap_used = 0;
    if (g_asi && g_asi->mem && g_asi->mem->info)
        g_asi->mem->info(&mi);

    uint64_t used_b = mi.used;
    uint64_t tot_b = mi.total;
    if (oom_lvl == ASI_OOM_OK) {
        used_b += mi.heap_used;
        tot_b += mi.heap_total;
    } else if (tot_b == 0) {
        used_b = mi.heap_used;
        tot_b = mi.heap_total;
    }

    uint64_t used_mb = used_b / (1024ull * 1024ull);
    uint64_t tot_mb = tot_b / (1024ull * 1024ull);
    if (tot_mb == 0) tot_mb = 1;
    uint64_t pct = (used_b * 100ull) / tot_b;
    if (pct > 100ull) pct = 100ull;
    if (oom_lvl >= ASI_OOM_CRITICAL && pct < 95ull) pct = 97ull;

    if (s_ram_text) {
        char buf[64];
        int n = 0;
        buf[0] = 0;
        oom_buf_u64(buf, sizeof(buf), &n, used_mb);
        oom_buf_cat(buf, sizeof(buf), &n, " MB / ");
        oom_buf_u64(buf, sizeof(buf), &n, tot_mb);
        oom_buf_cat(buf, sizeof(buf), &n, " MB (%");
        oom_buf_u64(buf, sizeof(buf), &n, pct);
        oom_buf_cat(buf, sizeof(buf), &n, ")");
        lv_label_set_text(s_ram_text, buf);
    }

    uint32_t bar_col = (oom_lvl >= ASI_OOM_CRITICAL || pct >= 85u) ? 0xEF4444
                     : (oom_lvl == ASI_OOM_LOW || pct >= 65u)      ? 0xF59E0B
                                                                   : 0x22C55E;
    uint32_t badge_col = (oom_lvl >= ASI_OOM_CRITICAL) ? 0xEF4444
                       : (oom_lvl == ASI_OOM_LOW)      ? 0xF59E0B
                       : (s_killed_any ? 0x16A34A : 0xD97706);

    if (s_ram_fill && s_ram_track) {
        int tw = lv_obj_get_width(s_ram_track);
        if (tw <= 0) tw = 540;
        int fw = (int)((uint64_t)tw * pct / 100ull);
        if (fw < 8 && pct > 0) fw = 8;
        if (fw > tw) fw = tw;
        lv_obj_set_width(s_ram_fill, fw);
        lv_obj_set_style_bg_color(s_ram_fill, lv_color_hex(bar_col), 0);
    }

    if (s_ram_badge && s_ram_badge_lbl) {
        const char *st_txt = (oom_lvl >= ASI_OOM_CRITICAL) ? "KRITIK"
                           : (oom_lvl == ASI_OOM_LOW)      ? "YUKSEK"
                           : (s_killed_any ? "RAHAT" : "DONDURULDU");
        lv_label_set_text(s_ram_badge_lbl, st_txt);
        lv_obj_set_style_bg_color(s_ram_badge, lv_color_hex(badge_col), 0);
    }

    if (s_btn_resume) {
        int can = oom_can_resume();
        if (can) {
            lv_obj_remove_state(s_btn_resume, LV_STATE_DISABLED);
            lv_obj_set_style_bg_color(s_btn_resume, lv_color_hex(ADL_ACCENT), 0);
            lv_obj_set_style_bg_color(s_btn_resume, lv_color_hex(0x2563EB), LV_STATE_HOVERED);
            lv_obj_set_style_border_width(s_btn_resume, 0, 0);
        } else {
            lv_obj_add_state(s_btn_resume, LV_STATE_DISABLED);
            lv_obj_set_style_bg_color(s_btn_resume, lv_color_hex(ADL_SURFACE_2), 0);
            lv_obj_set_style_border_width(s_btn_resume, 1, 0);
            lv_obj_set_style_border_color(s_btn_resume, lv_color_hex(ADL_BORDER), 0);
        }
    }
}

static void oom_update_selection_visuals(void) {
    if (!s_list) return;
    uint32_t cnt = lv_obj_get_child_count(s_list);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *row = lv_obj_get_child(s_list, (int32_t)i);
        if (!row) continue;
        if ((int)i == s_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(0x23293A), 0);
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_set_style_border_color(row, lv_color_hex(ADL_ACCENT), 0);
            lv_obj_scroll_to_view(row, LV_ANIM_OFF);
        } else {
            lv_obj_set_style_bg_color(row, lv_color_hex(ADL_SURFACE_2), 0);
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_set_style_border_color(row, lv_color_hex(0x232834), 0);
        }
    }
    if (s_oom_group && s_sel >= 0 && s_sel < s_item_n && s_row_btns[s_sel])
        lv_group_focus_obj(s_row_btns[s_sel]);
}

static void oom_row_click_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < s_item_n) {
        s_sel = idx;
        oom_update_selection_visuals();
    }
}

static void oom_kill_btn_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_pending_kill < 0) s_pending_kill = idx;
}

static void oom_resume_cb(lv_event_t *e) {
    (void)e;
    if (!oom_can_resume()) return;
    s_pending_exit = 1;
}

static void oom_reboot_cb(lv_event_t *e) {
    (void)e;
    oom_log("[oom-ui] user requested system reboot\n");
    if (g_asi && g_asi->system && g_asi->system->reboot)
        g_asi->system->reboot();
}

static void oom_key_cb(lv_event_t *e) {
    uint32_t k = lv_event_get_key(e);
    if (k == LV_KEY_UP || k == 'w' || k == 'W' || k == 'k' || k == 'K') {
        if (s_item_n > 0) {
            s_sel = (s_sel > 0) ? (s_sel - 1) : (s_item_n - 1);
            oom_update_selection_visuals();
        }
    } else if (k == LV_KEY_DOWN || k == 's' || k == 'S' || k == 'j' || k == 'J') {
        if (s_item_n > 0) {
            s_sel = (s_sel + 1) % s_item_n;
            oom_update_selection_visuals();
        }
    } else if (k == LV_KEY_DEL || k == LV_KEY_BACKSPACE || k == 'x' || k == 'X') {
        if (s_item_n > 0 && s_pending_kill < 0) s_pending_kill = s_sel;
    } else if (k == LV_KEY_ESC) {
        if (oom_can_resume()) s_pending_exit = 1;
    }
}

static void oom_rebuild_list(void) {
    if (!s_list) return;
    lv_obj_clean(s_list);
    if (s_oom_group) lv_group_remove_all_objs(s_oom_group);
    for (int i = 0; i < OOM_UI_MAX_ITEMS; i++) s_row_btns[i] = 0;

    if (s_sec_lbl) {
        char sh[80];
        int sn = 0;
        sh[0] = 0;
        oom_buf_cat(sh, sizeof(sh), &sn, "Acik Uygulamalar (");
        oom_buf_u64(sh, sizeof(sh), &sn, (uint64_t)s_item_n);
        oom_buf_cat(sh, sizeof(sh), &sn, " gorev)");
        lv_label_set_text(s_sec_lbl, sh);
    }

    if (s_item_n == 0) {
        lv_obj_t *empty = label(s_list, "Kapatilabilir baska uygulama yok.", ADL_TEXT_DIM);
        lv_obj_set_style_text_font(empty, s_f_sub ? s_f_sub : adl_font(), 0);
        lv_obj_set_style_pad_top(empty, 48, 0);
        lv_obj_set_width(empty, LV_PCT(100));
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    } else {
        for (int i = 0; i < s_item_n; i++) {
            oom_item_t *it = &s_items[i];
            int sel = (i == s_sel);

            lv_obj_t *row = lv_obj_create(s_list);
            lv_obj_remove_style_all(row);
            lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_size(row, LV_PCT(100), 48);
            lv_obj_set_style_radius(row, 8, 0);
            lv_obj_set_style_bg_color(row, lv_color_hex(sel ? 0x23293A : ADL_SURFACE_2), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(row, lv_color_hex(0x212636), LV_STATE_HOVERED);
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_set_style_border_color(row, lv_color_hex(sel ? ADL_ACCENT : 0x232834), 0);
            lv_obj_set_style_pad_left(row, 12, 0);
            lv_obj_set_style_pad_right(row, 10, 0);
            lv_obj_add_event_cb(row, oom_row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

            /* Sol: Uygulama adi + alt durum (PID / Donduruldu) */
            lv_obj_t *nm = label(row, it->name, ADL_TEXT);
            lv_obj_set_style_text_font(nm, s_f_sub ? s_f_sub : adl_font(), 0);
            lv_obj_set_width(nm, 220);
            lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
            lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 0, 6);

            char meta[64];
            int mn = 0;
            meta[0] = 0;
            if (it->pid > 1) {
                oom_buf_cat(meta, sizeof(meta), &mn, "PID ");
                oom_buf_u64(meta, sizeof(meta), &mn, (uint64_t)it->pid);
                oom_buf_cat(meta, sizeof(meta), &mn, " - ");
            }
            oom_buf_cat(meta, sizeof(meta), &mn, it->frozen ? "Donduruldu" : "Calisiyor");
            lv_obj_t *ml = label(row, meta, ADL_TEXT_DIM);
            lv_obj_set_style_text_font(ml, s_f_small ? s_f_small : adl_font(), 0);
            lv_obj_align(ml, LV_ALIGN_BOTTOM_LEFT, 0, -5);

            /* Orta-sag: Tur rozeti + RAM miktari */
            lv_obj_t *badge = lv_obj_create(row);
            lv_obj_remove_style_all(badge);
            lv_obj_remove_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_size(badge, 74, 22);
            lv_obj_set_style_radius(badge, 6, 0);
            lv_obj_set_style_bg_color(badge, lv_color_hex(oom_kind_color(it->kind)), 0);
            lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(badge, 1, 0);
            lv_obj_set_style_border_color(badge, lv_color_hex(oom_kind_border(it->kind)), 0);
            lv_obj_align(badge, LV_ALIGN_RIGHT_MID, -172, 0);
            {
                lv_obj_t *bl = label(badge, oom_kind_label(it->kind), ADL_TEXT);
                lv_obj_set_style_text_font(bl, s_f_small ? s_f_small : adl_font(), 0);
                lv_obj_center(bl);
            }

            char rbuf[24];
            oom_fmt_ram(rbuf, sizeof(rbuf), it->ram_kb);
            lv_obj_t *rl = label(row, rbuf, ADL_TEXT);
            lv_obj_set_style_text_font(rl, s_f_small ? s_f_small : adl_font(), 0);
            lv_obj_set_width(rl, 66);
            lv_obj_set_style_text_align(rl, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_align(rl, LV_ALIGN_RIGHT_MID, -98, 0);

            /* Sag: Kirmizi [Sonlandir] dugmesi */
            lv_obj_t *kb = lv_button_create(row);
            lv_obj_set_size(kb, 88, 30);
            lv_obj_set_style_radius(kb, 6, 0);
            lv_obj_set_style_bg_color(kb, lv_color_hex(0xC42B1C), 0);
            lv_obj_set_style_bg_color(kb, lv_color_hex(0xEF4444), LV_STATE_HOVERED);
            lv_obj_set_style_bg_color(kb, lv_color_hex(0x991B1B), LV_STATE_PRESSED);
            lv_obj_set_style_border_width(kb, 0, 0);
            lv_obj_align(kb, LV_ALIGN_RIGHT_MID, 0, 0);
            {
                lv_obj_t *kl = label(kb, "Sonlandir", 0xFFFFFF);
                lv_obj_set_style_text_font(kl, s_f_small ? s_f_small : adl_font(), 0);
                lv_obj_center(kl);
            }
            lv_obj_add_event_cb(kb, oom_kill_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_add_event_cb(kb, oom_key_cb, LV_EVENT_KEY, (void *)(intptr_t)i);
            s_row_btns[i] = kb;

            if (s_oom_group) lv_group_add_obj(s_oom_group, kb);
        }
    }

    if (s_oom_group) {
        if (s_btn_resume) lv_group_add_obj(s_oom_group, s_btn_resume);
        if (s_btn_reboot) lv_group_add_obj(s_oom_group, s_btn_reboot);
        if (s_item_n > 0 && s_sel >= 0 && s_sel < s_item_n && s_row_btns[s_sel])
            lv_group_focus_obj(s_row_btns[s_sel]);
        else if (s_btn_resume && oom_can_resume())
            lv_group_focus_obj(s_btn_resume);
        else if (s_btn_reboot)
            lv_group_focus_obj(s_btn_reboot);
    }
}

void oom_ui_init(void) {
    s_open = 0;
    s_manual = 0;
    s_sel = 0;
    s_killed_any = 0;
}

int oom_ui_is_open(void) {
    return s_open;
}

void oom_ui_reset_after_rebuild(void) {
    s_ov = 0;
    s_panel = 0;
    s_ram_text = 0;
    s_ram_badge = 0;
    s_ram_badge_lbl = 0;
    s_ram_fill = 0;
    s_ram_track = 0;
    s_sec_lbl = 0;
    s_list = 0;
    s_status_lbl = 0;
    s_btn_resume = 0;
    s_btn_reboot = 0;
    s_open = 0;
    s_manual = 0;
}

void oom_ui_enter(int manual) {
    uint64_t now = (g_asi && g_asi->time && g_asi->time->ticks_ms)
                   ? g_asi->time->ticks_ms() : 0;
    if (manual) {
        if (now && s_last_hotkey_ms && (now - s_last_hotkey_ms < 500u)) return;
        s_last_hotkey_ms = now;
    }
    if (s_open) return;

    s_open = 1;
    s_manual = manual ? 1 : 0;
    s_sel = 0;
    s_killed_any = 0;
    s_pending_kill = -1;
    s_pending_exit = 0;
    s_last_refresh_ms = now;
    if (g_start_menu && !lv_obj_has_flag(g_start_menu, LV_OBJ_FLAG_HIDDEN))
        lv_obj_add_flag(g_start_menu, LV_OBJ_FLAG_HIDDEN);

    /* Tum calisan kullanici/WLF/POSIX gorevlerini guvenle dondur */
    if (g_asi && g_asi->proc && g_asi->proc->oom_freeze_apps)
        g_asi->proc->oom_freeze_apps();

    oom_log("[oom-ui] entering RAM Doldu! recovery screen (apps frozen)\n");

    if (!s_f_title) s_f_title = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 20);
    if (!s_f_sub)   s_f_sub   = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 13);
    if (!s_f_small) s_f_small = lv_tiny_ttf_create_data(font_ttf, font_ttf_len, 12);

    int sw = (g_asi && g_asi->gfx && g_asi->gfx->width) ? g_asi->gfx->width() : 1280;
    int sh = (g_asi && g_asi->gfx && g_asi->gfx->height) ? g_asi->gfx->height() : 720;
    int cw = 620;
    int ch = 490;
    if (cw > sw - 40) cw = sw - 40;
    if (ch > sh - 40) ch = sh - 40;

    /* Power-menu tarzi koyu yari saydam arka plan perdesi */
    s_ov = lv_obj_create(lv_screen_active());
    base_obj(s_ov, 0x000000, 0, 0x000000);
    lv_obj_set_style_bg_opa(s_ov, 175, 0);
    lv_obj_set_style_border_width(s_ov, 0, 0);
    lv_obj_set_size(s_ov, LV_PCT(100), LV_PCT(100));
    lv_obj_align(s_ov, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_ov, LV_OBJ_FLAG_CLICKABLE);

    /* Ortalanmis sik koyu kart (power.c estetiği) */
    s_panel = lv_obj_create(s_ov);
    base_obj(s_panel, ADL_SURFACE, 14, ADL_BORDER);
    lv_obj_set_style_shadow_width(s_panel, 30, 0);
    lv_obj_set_style_shadow_color(s_panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(s_panel, 190, 0);
    lv_obj_set_style_pad_all(s_panel, 0, 0);
    lv_obj_set_style_clip_corner(s_panel, true, 0);
    lv_obj_set_size(s_panel, cw, ch);
    lv_obj_align(s_panel, LV_ALIGN_CENTER, 0, 0);

    /* Ust uyari seridi */
    lv_obj_t *strip = lv_obj_create(s_panel);
    lv_obj_remove_style_all(strip);
    lv_obj_set_size(strip, cw, 5);
    lv_obj_set_pos(strip, 0, 0);
    lv_obj_set_style_bg_color(strip, lv_color_hex(0xEF4444), 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);

    /* Baslik ikonu + "RAM Doldu!" */
    lv_obj_t *ic = make_icon(s_panel, SVG_OOM_WARN, 38);
    lv_obj_set_pos(ic, 24, 20);

    lv_obj_t *ttl = label(s_panel, "RAM Doldu!", ADL_TEXT);
    lv_obj_set_style_text_font(ttl, s_f_title ? s_f_title : adl_font(), 0);
    lv_obj_set_pos(ttl, 74, 18);

    lv_obj_t *sub = label(s_panel,
                          "Sistem bellegi kritik seviyede. Uygulamalar gecici olarak donduruldu.",
                          ADL_TEXT_DIM);
    lv_obj_set_style_text_font(sub, s_f_sub ? s_f_sub : adl_font(), 0);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(sub, cw - 102);
    lv_obj_set_pos(sub, 74, 46);

    /* Canli bellek kullanim kutusu */
    int inner_w = cw - 48;
    lv_obj_t *mbox = lv_obj_create(s_panel);
    base_obj(mbox, 0x0F1115, 10, ADL_BORDER);
    lv_obj_set_style_pad_all(mbox, 10, 0);
    lv_obj_set_size(mbox, inner_w, 56);
    lv_obj_set_pos(mbox, 24, 92);

    lv_obj_t *mlbl = label(mbox, "Bellek Kullanimi", ADL_TEXT_DIM);
    lv_obj_set_style_text_font(mlbl, s_f_small ? s_f_small : adl_font(), 0);
    lv_obj_align(mlbl, LV_ALIGN_TOP_LEFT, 2, 0);

    s_ram_badge = lv_obj_create(mbox);
    lv_obj_remove_style_all(s_ram_badge);
    lv_obj_set_size(s_ram_badge, 92, 20);
    lv_obj_set_style_radius(s_ram_badge, 5, 0);
    lv_obj_set_style_bg_color(s_ram_badge, lv_color_hex(0xEF4444), 0);
    lv_obj_set_style_bg_opa(s_ram_badge, LV_OPA_COVER, 0);
    lv_obj_align(s_ram_badge, LV_ALIGN_TOP_RIGHT, -2, -2);
    s_ram_badge_lbl = label(s_ram_badge, "KRITIK", 0xFFFFFF);
    lv_obj_set_style_text_font(s_ram_badge_lbl, s_f_small ? s_f_small : adl_font(), 0);
    lv_obj_center(s_ram_badge_lbl);

    s_ram_text = label(mbox, "0 MB / 0 MB (%0)", ADL_TEXT);
    lv_obj_set_style_text_font(s_ram_text, s_f_small ? s_f_small : adl_font(), 0);
    lv_obj_align(s_ram_text, LV_ALIGN_TOP_RIGHT, -104, 0);

    s_ram_track = lv_obj_create(mbox);
    lv_obj_remove_style_all(s_ram_track);
    lv_obj_set_size(s_ram_track, inner_w - 24, 8);
    lv_obj_set_style_radius(s_ram_track, 4, 0);
    lv_obj_set_style_bg_color(s_ram_track, lv_color_hex(0x222632), 0);
    lv_obj_set_style_bg_opa(s_ram_track, LV_OPA_COVER, 0);
    lv_obj_align(s_ram_track, LV_ALIGN_BOTTOM_LEFT, 2, -2);

    s_ram_fill = lv_obj_create(s_ram_track);
    lv_obj_remove_style_all(s_ram_fill);
    lv_obj_set_size(s_ram_fill, 12, 8);
    lv_obj_set_style_radius(s_ram_fill, 4, 0);
    lv_obj_set_style_bg_color(s_ram_fill, lv_color_hex(0xEF4444), 0);
    lv_obj_set_style_bg_opa(s_ram_fill, LV_OPA_COVER, 0);
    lv_obj_set_pos(s_ram_fill, 0, 0);

    /* Liste basligi */
    s_sec_lbl = label(s_panel, "Acik Uygulamalar", ADL_TEXT_DIM);
    lv_obj_set_style_text_font(s_sec_lbl, s_f_small ? s_f_small : adl_font(), 0);
    lv_obj_set_pos(s_sec_lbl, 26, 158);

    /* Kaydirilabilir uygulama listesi */
    int list_h = ch - 252;
    if (list_h < 120) list_h = 120;
    s_list = lv_obj_create(s_panel);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, inner_w, list_h);
    lv_obj_set_pos(s_list, 24, 180);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(0x0F1115), 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_list, 10, 0);
    lv_obj_set_style_border_width(s_list, 1, 0);
    lv_obj_set_style_border_color(s_list, lv_color_hex(ADL_BORDER), 0);
    lv_obj_set_style_pad_all(s_list, 8, 0);
    lv_obj_set_style_pad_row(s_list, 6, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);

    /* Alt durum + eylem cubugu */
    s_status_lbl = label(s_panel, "Secip Sonlandir'a tiklayin.", ADL_TEXT_DIM);
    lv_obj_set_style_text_font(s_status_lbl, s_f_small ? s_f_small : adl_font(), 0);
    lv_obj_set_width(s_status_lbl, inner_w - 384);
    lv_label_set_long_mode(s_status_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(s_status_lbl, LV_ALIGN_BOTTOM_LEFT, 24, -31);

    s_btn_resume = lv_button_create(s_panel);
    lv_obj_set_size(s_btn_resume, 166, 38);
    lv_obj_set_style_radius(s_btn_resume, 8, 0);
    lv_obj_align(s_btn_resume, LV_ALIGN_BOTTOM_RIGHT, -228, -20);
    {
        lv_obj_t *rl = label(s_btn_resume, "Masaustune Don", ADL_TEXT);
        lv_obj_set_style_text_font(rl, s_f_sub ? s_f_sub : adl_font(), 0);
        lv_obj_center(rl);
    }
    lv_obj_add_event_cb(s_btn_resume, oom_resume_cb, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(s_btn_resume, oom_key_cb, LV_EVENT_KEY, 0);

    s_btn_reboot = lv_button_create(s_panel);
    lv_obj_set_size(s_btn_reboot, 196, 38);
    lv_obj_set_style_radius(s_btn_reboot, 8, 0);
    lv_obj_set_style_bg_color(s_btn_reboot, lv_color_hex(0xC42B1C), 0);
    lv_obj_set_style_bg_color(s_btn_reboot, lv_color_hex(0xEF4444), LV_STATE_HOVERED);
    lv_obj_set_style_border_width(s_btn_reboot, 0, 0);
    lv_obj_align(s_btn_reboot, LV_ALIGN_BOTTOM_RIGHT, -24, -20);
    {
        lv_obj_t *rbl = label(s_btn_reboot, "Sistemi Yeniden Baslat", 0xFFFFFF);
        lv_obj_set_style_text_font(rbl, s_f_sub ? s_f_sub : adl_font(), 0);
        lv_obj_center(rbl);
    }
    lv_obj_add_event_cb(s_btn_reboot, oom_reboot_cb, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(s_btn_reboot, oom_key_cb, LV_EVENT_KEY, 0);

    if (!s_oom_group) s_oom_group = lv_group_create();
    lv_indev_t *kp = lv_port_get_keypad();
    if (kp && s_oom_group) lv_indev_set_group(kp, s_oom_group);

    oom_collect_items();
    oom_refresh_ram_bar();
    oom_rebuild_list();

    lv_obj_move_foreground(s_ov);
}

void oom_ui_exit(void) {
    if (!s_open) return;

    if (g_asi && g_asi->proc && g_asi->proc->oom_thaw_apps)
        g_asi->proc->oom_thaw_apps();

    oom_log("[oom-ui] leaving RAM Doldu! screen (apps thawed)\n");

    if (s_oom_group) lv_group_remove_all_objs(s_oom_group);
    if (s_ov) {
        lv_obj_delete(s_ov);
        s_ov = 0;
    }
    s_panel = 0;
    s_ram_text = 0;
    s_ram_badge = 0;
    s_ram_badge_lbl = 0;
    s_ram_fill = 0;
    s_ram_track = 0;
    s_sec_lbl = 0;
    s_list = 0;
    s_status_lbl = 0;
    s_btn_resume = 0;
    s_btn_reboot = 0;
    s_open = 0;
    s_manual = 0;

    lv_indev_t *kp = lv_port_get_keypad();
    if (kp) {
        winrec_t *topw = winrec_top();
        lv_group_t *want = (topw && topw->fgroup &&
                            lv_group_get_obj_count(topw->fgroup) > 0)
                           ? topw->fgroup : g_desktop_group;
        if (want) lv_indev_set_group(kp, want);
    }
}

void oom_ui_tick(void) {
    int oom_lvl = (g_asi && g_asi->proc && g_asi->proc->oom_state)
                  ? g_asi->proc->oom_state() : ASI_OOM_OK;

    if (!s_open) {
        s_pending_kill = -1;
        s_pending_exit = 0;
        if (oom_lvl >= ASI_OOM_CRITICAL)
            oom_ui_enter(0);
        return;
    }

    /* LVGL olay dagitimi disinda guvenli ertelemeli sonlandirma / cikis */
    if (s_pending_exit) {
        s_pending_exit = 0;
        s_pending_kill = -1;
        oom_ui_exit();
        return;
    }
    if (s_pending_kill >= 0) {
        int k = s_pending_kill;
        s_pending_kill = -1;
        oom_kill_index(k);
    }

    /* Acik oldugu surece klavye odagini OOM grubunda tut */
    lv_indev_t *kp = lv_port_get_keypad();
    if (kp && s_oom_group && lv_indev_get_group(kp) != s_oom_group)
        lv_indev_set_group(kp, s_oom_group);

    if (s_ov) lv_obj_move_foreground(s_ov);

    uint64_t now = (g_asi && g_asi->time && g_asi->time->ticks_ms)
                   ? g_asi->time->ticks_ms() : 0;
    if (now - s_last_refresh_ms >= 500u) {
        s_last_refresh_ms = now;
        oom_refresh_ram_bar();
    }
}
