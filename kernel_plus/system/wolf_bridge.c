/* B3: izole .wlf GUI koprusu — syscall dispatcher + olay kuyrugu + broker.
 *
 * Trampoline stublari (main_kernel/wlf_user.c) 8 C argumanini kullanici
 * argbuf'ina yazar ve SYS_WOLF_CALL+indeks ile syscall yapar. Burasi:
 *  - argumanlari kullanici alanindan dogrular/kopyalar (bounce),
 *  - LVGL'siz API'leri dogrudan, LVGL'li API'leri masaustu broker
 *    kuyrugunda (LVGL baglaminda) yurutur,
 *  - ring-3 IF=0 oldugu icin bekleme/sleep/ag sirasinda cekirdek CR3'une
 *    gecip sti acar, donuste uygulama CR3'une doner,
 *  - olaylari (click/change/timer/resize) kullanici olay dongusune tasir.
 *
 * Bekleme yoklama (poll) ile: kayip-uyanima (lost wakeup) kilidi gerektirmez;
 * olay gecikmesi ~5 ms, broker cagri gecikmesi ~2 ms (B3 v1 kabul).
 */
#include "wolf_bridge.h"
#include "asi.h"
#include "wolf.h"
#include "sched.h"
#include "vmm_space.h"
#include "alloc.h"
#include "kernel.h"
#include "serial.h"

extern asi_t *g_asi;

/* ---- arguman turu / mod / donus ---- */
enum { K_INT = 0, K_CSTR, K_BUFIN, K_BUFOUT, K_INTOUT, K_CB, K_USER };
enum { M_BROKER = 0, M_DIRECT, M_SAFE, M_SLEEP };
enum { R_INT = 0, R_CSTR };

typedef struct {
    uint8_t mode, ret, outstr;
    uint8_t k[8];
    uint8_t lp[8];   /* BUF* icin uzunluk argumaninin indeksi (1 tabanli) */
} wmeta_t;

#define Z8 {0,0,0,0,0,0,0,0}
static const wmeta_t META[94] = {
/*0 root          */ {M_BROKER,R_INT,0,Z8,Z8},
/*1 win_title     */ {M_BROKER,R_INT,0,{K_CSTR},Z8},
/*2 win_size      */ {M_BROKER,R_INT,0,Z8,Z8},
/*3 panel         */ {M_BROKER,R_INT,0,Z8,Z8},
/*4 label         */ {M_BROKER,R_INT,0,{0,0,0,K_CSTR},Z8},
/*5 button        */ {M_BROKER,R_INT,0,{0,0,0,0,0,K_CSTR},Z8},
/*6 input         */ {M_BROKER,R_INT,0,{0,0,0,0,0,K_CSTR},Z8},
/*7 listbox       */ {M_BROKER,R_INT,0,Z8,Z8},
/*8 image         */ {M_BROKER,R_INT,0,{0,0,0,0,0,K_CSTR},Z8},
/*9 progress      */ {M_BROKER,R_INT,0,Z8,Z8},
/*10 slider       */ {M_BROKER,R_INT,0,Z8,Z8},
/*11 separator    */ {M_BROKER,R_INT,0,Z8,Z8},
/*12 checkbox     */ {M_BROKER,R_INT,0,{0,0,0,K_CSTR},Z8},
/*13 set_text     */ {M_BROKER,R_INT,0,{0,K_CSTR},Z8},
/*14 get_text     */ {M_BROKER,R_INT,1,{0,K_BUFOUT},{0,3}},
/*15 set_value    */ {M_BROKER,R_INT,0,Z8,Z8},
/*16 get_value    */ {M_BROKER,R_INT,0,Z8,Z8},
/*17 set_pos      */ {M_BROKER,R_INT,0,Z8,Z8},
/*18 set_size     */ {M_BROKER,R_INT,0,Z8,Z8},
/*19 set_color    */ {M_BROKER,R_INT,0,Z8,Z8},
/*20 set_visible  */ {M_BROKER,R_INT,0,Z8,Z8},
/*21 set_enabled  */ {M_BROKER,R_INT,0,Z8,Z8},
/*22 clear        */ {M_BROKER,R_INT,0,Z8,Z8},
/*23 destroy      */ {M_BROKER,R_INT,0,Z8,Z8},
/*24 on_click     */ {M_BROKER,R_INT,0,{0,K_CB,K_USER},Z8},
/*25 on_change    */ {M_BROKER,R_INT,0,{0,K_CB,K_USER},Z8},
/*26 list_add     */ {M_BROKER,R_INT,0,{0,K_CSTR},Z8},
/*27 list_clear   */ {M_BROKER,R_INT,0,Z8,Z8},
/*28 list_count   */ {M_BROKER,R_INT,0,Z8,Z8},
/*29 list_item    */ {M_BROKER,R_INT,1,{0,0,K_BUFOUT},{0,0,4}},
/*30 list_selected*/ {M_BROKER,R_INT,0,Z8,Z8},
/*31 fs_read      */ {M_DIRECT,R_INT,0,{K_CSTR,K_BUFOUT},{0,3}},
/*32 fs_write     */ {M_DIRECT,R_INT,0,{K_CSTR,K_BUFIN},{0,3}},
/*33 fs_list      */ {M_DIRECT,R_INT,1,{K_CSTR,0,K_BUFOUT,0,K_INTOUT},{0,0,4}},
/*34 fs_count     */ {M_DIRECT,R_INT,0,{K_CSTR},Z8},
/*35 fs_exists    */ {M_DIRECT,R_INT,0,{K_CSTR},Z8},
/*36 fs_remove    */ {M_DIRECT,R_INT,0,{K_CSTR},Z8},
/*37 fs_mkdir     */ {M_DIRECT,R_INT,0,{K_CSTR},Z8},
/*38 fs_rename    */ {M_DIRECT,R_INT,0,{K_CSTR,K_CSTR},Z8},
/*39 log          */ {M_DIRECT,R_INT,0,{K_CSTR},Z8},
/*40 notify       */ {M_BROKER,R_INT,0,{K_CSTR,K_CSTR},Z8},
/*41 dialog       */ {M_BROKER,R_INT,0,{K_CSTR,K_CSTR,K_CSTR},Z8},
/*42 exec         */ {M_BROKER,R_INT,1,{K_CSTR,K_BUFOUT},{0,3}},
/*43 ticks_ms     */ {M_DIRECT,R_INT,0,Z8,Z8},
/*44 sleep_ms     */ {M_SLEEP,R_INT,0,Z8,Z8},
/*45 now          */ {M_DIRECT,R_INT,0,{K_INTOUT,K_INTOUT,K_INTOUT,K_INTOUT,K_INTOUT,K_INTOUT},Z8},
/*46 os_version   */ {M_DIRECT,R_INT,1,{K_BUFOUT},{2}},
/*47 open_app     */ {M_BROKER,R_INT,0,{K_CSTR},Z8},
/*48 clip_set     */ {M_DIRECT,R_INT,0,{K_CSTR},Z8},
/*49 clip_get     */ {M_DIRECT,R_INT,1,{K_BUFOUT},{2}},
/*50 fetch        */ {M_SAFE,R_INT,0,{K_CSTR,K_BUFOUT,0,K_INTOUT},{0,3}},
/*51 arg          */ {M_DIRECT,R_CSTR,0,Z8,Z8},
/*52 style        */ {M_BROKER,R_INT,0,Z8,Z8},
/*53 line         */ {M_BROKER,R_INT,0,Z8,Z8},
/*54 line_reset   */ {M_BROKER,R_INT,0,Z8,Z8},
/*55 line_to      */ {M_BROKER,R_INT,0,Z8,Z8},
/*56 arc          */ {M_BROKER,R_INT,0,Z8,Z8},
/*57 arc_set      */ {M_BROKER,R_INT,0,Z8,Z8},
/*58 canvas       */ {M_BROKER,R_INT,0,Z8,Z8},
/*59 canvas_px    */ {M_BROKER,R_INT,0,Z8,Z8},
/*60 canvas_rect  */ {M_BROKER,R_INT,0,Z8,Z8},
/*61 canvas_line  */ {M_BROKER,R_INT,0,Z8,Z8},
/*62 canvas_circle*/ {M_BROKER,R_INT,0,Z8,Z8},
/*63 canvas_grad  */ {M_BROKER,R_INT,0,Z8,Z8},
/*64 canvas_clear */ {M_BROKER,R_INT,0,Z8,Z8},
/*65 canvas_getpx */ {M_BROKER,R_INT,0,Z8,Z8},
/*66 audio_ok     */ {M_DIRECT,R_INT,0,Z8,Z8},
/*67 audio_play   */ {M_SAFE,R_INT,0,{K_CSTR},Z8},
/*68 audio_play_mem*/{M_SAFE,R_INT,0,{K_BUFIN},{2}},
/*69 audio_stop   */ {M_SAFE,R_INT,0,Z8,Z8},
/*70 audio_playing*/ {M_DIRECT,R_INT,0,Z8,Z8},
/*71 audio_vol    */ {M_DIRECT,R_INT,0,Z8,Z8},
/*72 audio_set_vol*/ {M_DIRECT,R_INT,0,Z8,Z8},
/*73 timer        */ {M_BROKER,R_INT,0,{0,K_CB,K_USER},Z8},
/*74 timer_cancel */ {M_BROKER,R_INT,0,Z8,Z8},
/*75 to_front     */ {M_BROKER,R_INT,0,Z8,Z8},
/*76 to_back      */ {M_BROKER,R_INT,0,Z8,Z8},
/*77 transition   */ {M_BROKER,R_INT,0,Z8,Z8},
/*78 state_set    */ {M_BROKER,R_INT,0,Z8,Z8},
/*79 anim_in      */ {M_BROKER,R_INT,0,Z8,Z8},
/*80 anim_pulse   */ {M_BROKER,R_INT,0,Z8,Z8},
/*81 screen       */ {M_BROKER,R_INT,0,{K_INTOUT,K_INTOUT},Z8},
/*82 icon         */ {M_BROKER,R_INT,0,{0,0,0,0,K_CSTR},Z8},
/*83 set_click    */ {M_BROKER,R_INT,0,Z8,Z8},
/*84 scrollable   */ {M_BROKER,R_INT,0,Z8,Z8},
/*85 scroll_to    */ {M_BROKER,R_INT,0,Z8,Z8},
/*86 win_close    */ {M_BROKER,R_INT,0,Z8,Z8},
/*87 win_min      */ {M_BROKER,R_INT,0,Z8,Z8},
/*88 on_resize    */ {M_BROKER,R_INT,0,{K_CB,K_USER},Z8},
/*89 open_app_file*/ {M_BROKER,R_INT,0,{K_CSTR,K_CSTR},Z8},
/*90 fs_size      */ {M_DIRECT,R_INT,0,{K_CSTR},Z8},
/*91 on_enter     */ {M_BROKER,R_INT,0,{0,K_CB,K_USER},Z8},
/*92 input_multi  */ {M_BROKER,R_INT,0,{0,0,0,0,0,K_CSTR},Z8},
/*93 ta_wrap      */ {M_BROKER,R_INT,0,{0,K_CSTR,K_CSTR},Z8},
};
#undef Z8

typedef uint64_t (*wfn8)(uint64_t, uint64_t, uint64_t, uint64_t,
                         uint64_t, uint64_t, uint64_t, uint64_t);

/* ---- tek aktif izole uygulama (wlf_user sozlesmesi: ic ice yok) ---- */
static struct {
    int          active;
    void        *body;
    wolf_api_t  *api;
    uint64_t     buf_va;
    uint64_t     kernel_cr3;
    sched_tid_t  tid;
} s_app;

/* broker: tek senkron cagri yuvasi (SPSC) */
static struct {
    volatile int pending;
    uint32_t     idx;
    uint64_t     a[8];
    int64_t      ret;
} s_req;

/* olay kuyrugu (SPSC ring) */
typedef struct { uint64_t cb, user; int64_t wid, kind; } wev_t;
static wev_t s_ev[32];
static volatile uint32_t s_ev_h, s_ev_t;

/* bounce tamponlari (cekirdek) */
static char b_str[4][1024];
static char b_buf[2][4096];
static int  b_int[8];

static int probe(uint64_t va, uint32_t len, int write) {
    if (len == 0) return 1;
    uint64_t end = va + len;
    if (end < va) return 0;
    vmm_space_t sp = vmm_space_current();
    for (uint64_t pg = va & ~0xFFFull; pg < end; pg += 0x1000ull)
        if (!vmm_probe_user(sp, pg, write)) return 0;
    return 1;
}

static int cstr_ok(uint64_t va) {
    if (!probe(va, 1, 0)) return 0;
    for (uint32_t n = 0; n < 1024u; n++, va++) {
        if ((va & 0xFFFu) == 0 && !probe(va, 1, 0)) return 0;
        if (*(const char *)(uintptr_t)va == 0) return 1;
    }
    return 0;
}

static uint64_t enter_kernel(void) {
    uint64_t sp = (uint64_t)vmm_space_current();
    vmm_space_switch((vmm_space_t)s_app.kernel_cr3);
    __asm__ volatile("sti" ::: "memory");
    return sp;
}

static void leave_kernel(uint64_t app_sp) {
    __asm__ volatile("cli" ::: "memory");
    vmm_space_switch((vmm_space_t)app_sp);
}

void wolf_bridge_attach(void *body, void *api, uint64_t buf_va) {
    s_app.body = body;
    s_app.api = (wolf_api_t *)api;
    s_app.buf_va = buf_va;
    s_app.tid = sched_current();
    s_app.kernel_cr3 = (uint64_t)vmm_space_current();
    s_req.pending = 0;
    s_ev_h = s_ev_t = 0;
    s_app.active = 1;
}

void wolf_bridge_detach(void) {
    s_app.active = 0;
    s_req.pending = 0;
    s_ev_h = s_ev_t = 0;
}

int wolf_bridge_event_push(void *body, uint64_t cb, uint64_t user,
                           int64_t wid, int64_t kind) {
    if (!s_app.active || body != s_app.body) return 0;
    uint32_t nh = (s_ev_h + 1u) % 32u;
    if (nh == s_ev_t) return 1;   /* kuyruk dolu: olay dustu (v1) */
    s_ev[s_ev_h].cb = cb;
    s_ev[s_ev_h].user = user;
    s_ev[s_ev_h].wid = wid;
    s_ev[s_ev_h].kind = kind;
    s_ev_h = nh;
    return 1;
}

void wolf_bridge_drain(void) {
    if (!s_app.active || !s_req.pending) return;
    /* host API'leri s_cur oturumuna bakar: dogru oturumu sec */
    if (g_asi && g_asi->wlf && g_asi->wlf->api_for_body)
        g_asi->wlf->api_for_body(s_app.body);
    uint32_t idx = s_req.idx;
    wfn8 f = (wfn8)((uint64_t *)s_app.api)[2 + idx];
    int64_t r = (int64_t)f(s_req.a[0], s_req.a[1], s_req.a[2], s_req.a[3],
                           s_req.a[4], s_req.a[5], s_req.a[6], s_req.a[7]);
    s_req.ret = r;
    s_req.pending = 0;
    if (idx == 86u)   /* win_close: uygulama dongusunu bitir */
        wolf_bridge_event_push(s_app.body, 0, 0, 0, 1);
}

int64_t wolf_bridge_wait(uint64_t buf_va) {
    if (!s_app.active || !probe(buf_va, 32, 1)) return -1;
    for (;;) {
        if (s_ev_t != s_ev_h) {
            wev_t e = s_ev[s_ev_t];
            s_ev_t = (s_ev_t + 1u) % 32u;
            uint64_t *u = (uint64_t *)(uintptr_t)buf_va;
            u[0] = e.cb; u[1] = e.user; u[2] = (uint64_t)e.wid; u[3] = (uint64_t)e.kind;
            return 0;
        }
        uint64_t sp = enter_kernel();
        sched_sleep_ms(5);
        leave_kernel(sp);
    }
}

int64_t wolf_bridge_call(uint32_t idx, uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!s_app.active || idx >= 94u || !s_app.api) return -1;
    const wmeta_t *m = &META[idx];

    /* argumanlari kullanici argbuf'undan oku (stub 8 arg yazar) */
    uint64_t ab = s_app.buf_va + 640;
    if (!probe(ab, 64, 0)) return -2;
    uint64_t ua[8];
    memcpy(ua, (const void *)(uintptr_t)ab, 64);

    uint64_t ka[8];
    int str_slot = 0, buf_slot = 0, int_slot = 0;
    int ob_idx = -1, ob_len = 0, ob_str = 0, ob_slot = 0, ob_lenint = -1;
    uint64_t ob_user = 0;
    int io_idx[8], io_n = 0;
    for (int i = 0; i < 8; i++) {
        ka[i] = ua[i];
        switch (m->k[i]) {
        case K_CSTR:
            if (!cstr_ok(ua[i]) || str_slot >= 4) return -2;
            { const char *s = (const char *)(uintptr_t)ua[i];
              int n = 0; while (s[n] && n < 1023) { b_str[str_slot][n] = s[n]; n++; }
              b_str[str_slot][n] = 0; }
            ka[i] = (uint64_t)(uintptr_t)b_str[str_slot++];
            break;
        case K_BUFIN: {
            uint32_t len = m->lp[i] ? (uint32_t)ua[m->lp[i] - 1] : 0;
            if (len > 4096 || buf_slot >= 2 || !probe(ua[i], len, 0)) return -2;
            memcpy(b_buf[buf_slot], (const void *)(uintptr_t)ua[i], len);
            ka[i] = (uint64_t)(uintptr_t)b_buf[buf_slot++];
            break;
        }
        case K_BUFOUT: {
            uint32_t len = m->lp[i] ? (uint32_t)ua[m->lp[i] - 1] : 0;
            if (len > 4096 || buf_slot >= 2 || !probe(ua[i], len, 1)) return -2;
            ob_idx = i; ob_len = (int)len; ob_str = m->outstr;
            ob_user = ua[i]; ob_slot = buf_slot; ob_lenint = -1;
            if (idx == 50u) ob_lenint = int_slot; /* fetch: len_out intout */
            ka[i] = (uint64_t)(uintptr_t)b_buf[buf_slot++];
            break;
        }
        case K_INTOUT:
            if (int_slot >= 8 || !probe(ua[i], 4, 1)) return -2;
            b_int[int_slot] = 0;
            io_idx[io_n++] = i;
            if (idx == 50u && i == 3) ob_lenint = int_slot;
            ka[i] = (uint64_t)(uintptr_t)&b_int[int_slot++];
            break;
        default: break;   /* K_INT / K_CB / K_USER: oldugu gibi */
        }
    }

    wfn8 f = (wfn8)((uint64_t *)s_app.api)[2 + idx];
    int64_t ret = -1;
    switch (m->mode) {
    case M_SLEEP: {
        uint64_t sp = enter_kernel();
        sched_sleep_ms((uint32_t)ua[0]);
        leave_kernel(sp);
        ret = 0;
        break;
    }
    case M_DIRECT:
        ret = (int64_t)f(ka[0], ka[1], ka[2], ka[3], ka[4], ka[5], ka[6], ka[7]);
        break;
    case M_SAFE: {
        uint64_t sp = enter_kernel();
        ret = (int64_t)f(ka[0], ka[1], ka[2], ka[3], ka[4], ka[5], ka[6], ka[7]);
        leave_kernel(sp);
        break;
    }
    default: {   /* M_BROKER: LVGL baglaminda masaustu yurutur */
        s_req.idx = idx;
        for (int i = 0; i < 8; i++) s_req.a[i] = ka[i];
        s_req.ret = -1;
        s_req.pending = 1;
        uint64_t sp = enter_kernel();
        while (s_req.pending) sched_sleep_ms(2);
        leave_kernel(sp);
        ret = s_req.ret;
        break;
    }
    }

    /* cikis tamponlarini kullaniciya geri kopyala */
    if (ob_idx >= 0 && ob_len > 0) {
        int n;
        if (ob_lenint >= 0) n = b_int[ob_lenint];
        else if (ob_str) { n = 0; while (n < ob_len && b_buf[ob_slot][n]) n++; }
        else n = (ret > 0 && ret <= ob_len) ? (int)ret : 0;
        if (n > ob_len) n = ob_len;
        if (n > 0 && probe((uint64_t)ob_user, (uint32_t)n, 1))
            memcpy((void *)(uintptr_t)ob_user, b_buf[ob_slot], (size_t)n);
    }
    for (int j = 0; j < io_n; j++) {
        int i = io_idx[j];
        if (probe(ua[i], 4, 1))
            *(int *)(uintptr_t)ua[i] = b_int[j];
    }

    if (m->ret == R_CSTR) {
        const char *s = (const char *)(uintptr_t)ret;
        uint64_t rb = s_app.buf_va + 64;
        if (!s || !probe(rb, 512, 1)) return -1;
        int n = 0;
        while (s[n] && n < 511) { ((char *)(uintptr_t)rb)[n] = s[n]; n++; }
        ((char *)(uintptr_t)rb)[n] = 0;
        return (int64_t)rb;
    }
    return ret;
}
