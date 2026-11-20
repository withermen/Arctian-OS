#include "ui_broker.h"
#include "sync.h"
#include "sched.h"
#include "clock.h"

/*
 * Tum broker durumu tek spinlock altinda. Kilit alinirken kesmeler de
 * kapatilir: olay kuyruguna surucu kesmesinden push edilebilir. Kanca
 * (ops) cagrilari kilit birakildiktan sonra yapilir; boylece LVGL geri
 * cagrilari broker'a serbestce girebilir (ic ice kilitlenme yok).
 */

#define UI_SLOT_BITS   12u
#define UI_SLOT_MASK   ((1u << UI_SLOT_BITS) - 1u)
#define UI_GEN_MASK    0x000FFFFFu

typedef struct {
    uint32_t      generation;
    ui_widget_id  parent;
    uint32_t      type;
    uint32_t      app;      /* UI_APP_NONE = sahibi yok */
    uint8_t       used;
} ui_widget_slot_t;

typedef struct {
    ui_widget_id id;
    uint32_t     type;
    int32_t      a;
    int32_t      b;
    uint32_t     app;       /* hedef uygulama (upcall icin) */
} ui_event_node_t;

static spinlock_t        g_lock;
static ui_broker_ops_t   g_ops;
static ui_widget_slot_t  g_widgets[UI_BROKER_WIDGET_MAX];
static uint32_t          g_widget_used;

static ui_event_node_t   g_events[UI_BROKER_EVENT_MAX];
static uint32_t          g_ev_head;
static uint32_t          g_ev_tail;
static uint32_t          g_ev_count;

static uint32_t          g_caps[UI_BROKER_APP_MAX];
static uint8_t           g_caps_known[UI_BROKER_APP_MAX];

static ui_upcall_fn      g_upcall[UI_BROKER_APP_MAX];
static void             *g_upcall_ctx[UI_BROKER_APP_MAX];

/* destroy tarafindan kullanilan gecici liste; tek seferde tek destroy. */
static ui_widget_id      g_destroy_buf[UI_BROKER_WIDGET_MAX];
static int               g_destroy_busy;

static uint64_t ui_lock(void) {
    uint64_t flags = sched_irq_save();
    spin_lock(&g_lock);
    return flags;
}

static void ui_unlock(uint64_t flags) {
    spin_unlock(&g_lock);
    sched_irq_restore(flags);
}

static ui_widget_id slot_id(uint32_t slot, uint32_t gen) {
    return ((gen & UI_GEN_MASK) << UI_SLOT_BITS) | (slot + 1u);
}

static int slot_of(ui_widget_id id, uint32_t *out_slot) {
    uint32_t raw = id & UI_SLOT_MASK;
    if (raw == 0 || raw > UI_BROKER_WIDGET_MAX) return 0;
    *out_slot = raw - 1u;
    return 1;
}

/* Kilit tutulurken cagrilir. */
static int widget_lookup(ui_widget_id id, uint32_t *out_slot) {
    uint32_t slot;
    if (id == UI_WIDGET_INVALID || !slot_of(id, &slot)) return 0;
    if (!g_widgets[slot].used) return 0;
    if (slot_id(slot, g_widgets[slot].generation) != id) return 0;
    if (out_slot) *out_slot = slot;
    return 1;
}

static void widget_snapshot_ops(ui_broker_ops_t *dst, uint64_t *flags) {
    *flags = ui_lock();
    *dst = g_ops;
}

void ui_broker_init(void) {
    spin_init(&g_lock);   /* kilit alinmadan once sifirla */
    uint64_t f = ui_lock();

    g_ops.ctx = 0;
    g_ops.create = 0;
    g_ops.label_set = 0;
    g_ops.button_set = 0;
    g_ops.input_get = 0;
    g_ops.destroy = 0;

    for (uint32_t i = 0; i < UI_BROKER_WIDGET_MAX; i++) {
        g_widgets[i].generation = 0;
        g_widgets[i].parent = UI_WIDGET_INVALID;
        g_widgets[i].type = 0;
        g_widgets[i].app = UI_APP_NONE;
        g_widgets[i].used = 0;
    }
    g_widget_used = 0;
    g_destroy_busy = 0;

    g_ev_head = g_ev_tail = g_ev_count = 0;

    for (uint32_t i = 0; i < UI_BROKER_APP_MAX; i++) {
        g_caps[i] = 0;
        g_caps_known[i] = 0;
        g_upcall[i] = 0;
        g_upcall_ctx[i] = 0;
    }

    ui_unlock(f);
}

int ui_broker_set_ops(const ui_broker_ops_t *ops) {
    uint64_t f = ui_lock();
    if (ops) g_ops = *ops;
    else {
        g_ops.ctx = 0;
        g_ops.create = 0;
        g_ops.label_set = 0;
        g_ops.button_set = 0;
        g_ops.input_get = 0;
        g_ops.destroy = 0;
    }
    ui_unlock(f);
    return UI_BROKER_OK;
}

/* ---------------------------------------------------------------- widget */

int ui_broker_widget_create(uint32_t type, ui_widget_id parent_id,
                            ui_widget_id *out_id) {
    if (!out_id) return UI_BROKER_ERR_PARAM;
    if (type >= UI_WIDGET_TYPE_MAX) return UI_BROKER_ERR_PARAM;
    if (type == UI_WIDGET_ROOT && parent_id != UI_WIDGET_INVALID)
        return UI_BROKER_ERR_PARAM;

    ui_broker_ops_t ops;
    ui_widget_id id = UI_WIDGET_INVALID;
    uint64_t f;
    int found = 0;

    widget_snapshot_ops(&ops, &f);

    if (parent_id != UI_WIDGET_INVALID && !widget_lookup(parent_id, 0)) {
        ui_unlock(f);
        return UI_BROKER_ERR_PARAM;
    }

    for (uint32_t i = 0; i < UI_BROKER_WIDGET_MAX; i++) {
        if (g_widgets[i].used) continue;
        g_widgets[i].used = 1;
        g_widgets[i].type = type;
        g_widgets[i].parent = parent_id;
        g_widgets[i].app = UI_APP_NONE;
        id = slot_id(i, g_widgets[i].generation);
        g_widget_used++;
        found = 1;
        break;
    }
    ui_unlock(f);

    if (!found) return UI_BROKER_ERR_FULL;

    if (ops.create) {
        int rc = ops.create(ops.ctx, id, type, parent_id);
        if (rc < 0) {
            uint32_t slot;
            f = ui_lock();
            if (widget_lookup(id, &slot)) {
                g_widgets[slot].used = 0;
                g_widgets[slot].parent = UI_WIDGET_INVALID;
                g_widgets[slot].generation =
                    (g_widgets[slot].generation + 1u) & UI_GEN_MASK;
                if (g_widget_used > 0) g_widget_used--;
            }
            ui_unlock(f);
            return rc;
        }
    }

    *out_id = id;
    return UI_BROKER_OK;
}

int ui_broker_widget_destroy(ui_widget_id id) {
    ui_broker_ops_t ops;
    uint64_t f;
    uint32_t slot;

    widget_snapshot_ops(&ops, &f);

    if (!widget_lookup(id, &slot)) {
        ui_unlock(f);
        return UI_BROKER_ERR_PARAM;
    }
    if (g_destroy_busy) {
        ui_unlock(f);
        return UI_BROKER_ERR_STATE;
    }
    g_destroy_busy = 1;

    /* Hedef + tum torunlari topla (slot sirasi = kabaca olusturma sirasi). */
    uint32_t n = 0;
    g_destroy_buf[n++] = id;
    for (uint32_t i = 0; i < n && n < UI_BROKER_WIDGET_MAX; i++) {
        ui_widget_id cur = g_destroy_buf[i];
        for (uint32_t s = 0; s < UI_BROKER_WIDGET_MAX; s++) {
            if (!g_widgets[s].used || g_widgets[s].parent != cur) continue;
            ui_widget_id child = slot_id(s, g_widgets[s].generation);
            int dup = 0;
            for (uint32_t k = 0; k < n; k++)
                if (g_destroy_buf[k] == child) { dup = 1; break; }
            if (dup) continue;
            if (n < UI_BROKER_WIDGET_MAX) g_destroy_buf[n++] = child;
        }
    }

    /* Kalan (liste tasmasi) cocuklarin ebeveynini gecersiz yap. */
    for (uint32_t s = 0; s < UI_BROKER_WIDGET_MAX; s++) {
        if (!g_widgets[s].used) continue;
        ui_widget_id pid = g_widgets[s].parent;
        int gone = 0;
        for (uint32_t k = 0; k < n; k++)
            if (g_destroy_buf[k] == pid) { gone = 1; break; }
        if (gone) g_widgets[s].parent = UI_WIDGET_INVALID;
    }

    for (uint32_t k = 0; k < n; k++) {
        if (!slot_of(g_destroy_buf[k], &slot)) continue;
        if (!g_widgets[slot].used) continue;
        g_widgets[slot].used = 0;
        g_widgets[slot].parent = UI_WIDGET_INVALID;
        g_widgets[slot].app = UI_APP_NONE;
        g_widgets[slot].generation =
            (g_widgets[slot].generation + 1u) & UI_GEN_MASK;
        if (g_widget_used > 0) g_widget_used--;
    }
    ui_unlock(f);

    /* Kancalar kilitsiz; cocuklar once (liste tersi) silinir. */
    if (ops.destroy) {
        for (uint32_t k = n; k > 0; k--)
            (void)ops.destroy(ops.ctx, g_destroy_buf[k - 1u]);
    }

    f = ui_lock();
    g_destroy_busy = 0;
    ui_unlock(f);
    return UI_BROKER_OK;
}

int ui_broker_widget_valid(ui_widget_id id) {
    uint64_t f = ui_lock();
    int ok = widget_lookup(id, 0);
    ui_unlock(f);
    return ok;
}

int ui_broker_widget_info(ui_widget_id id, uint32_t *out_type,
                          ui_widget_id *out_parent) {
    uint64_t f = ui_lock();
    uint32_t slot;
    if (!widget_lookup(id, &slot)) {
        ui_unlock(f);
        return UI_BROKER_ERR_PARAM;
    }
    if (out_type) *out_type = g_widgets[slot].type;
    if (out_parent) *out_parent = g_widgets[slot].parent;
    ui_unlock(f);
    return UI_BROKER_OK;
}

uint32_t ui_broker_widget_count(void) {
    uint64_t f = ui_lock();
    uint32_t n = g_widget_used;
    ui_unlock(f);
    return n;
}

/* --------------------------------------------------------------- kancalar */

int ui_broker_label_set(ui_widget_id id, const char *text) {
    if (!text) return UI_BROKER_ERR_PARAM;

    ui_broker_ops_t ops;
    uint64_t f;
    uint32_t slot;

    widget_snapshot_ops(&ops, &f);
    if (!widget_lookup(id, &slot)) {
        ui_unlock(f);
        return UI_BROKER_ERR_PARAM;
    }
    uint32_t type = g_widgets[slot].type;
    ui_unlock(f);

    if (type != UI_WIDGET_LABEL && type != UI_WIDGET_BUTTON)
        return UI_BROKER_ERR_PARAM;
    if (!ops.label_set) return UI_BROKER_OK;
    return ops.label_set(ops.ctx, id, text);
}

int ui_broker_button_set(ui_widget_id id, const char *text, int pressed) {
    if (!text) return UI_BROKER_ERR_PARAM;

    ui_broker_ops_t ops;
    uint64_t f;
    uint32_t slot;

    widget_snapshot_ops(&ops, &f);
    if (!widget_lookup(id, &slot)) {
        ui_unlock(f);
        return UI_BROKER_ERR_PARAM;
    }
    uint32_t type = g_widgets[slot].type;
    ui_unlock(f);

    if (type != UI_WIDGET_BUTTON) return UI_BROKER_ERR_PARAM;
    if (!ops.button_set) return UI_BROKER_OK;
    return ops.button_set(ops.ctx, id, text, pressed);
}

int ui_broker_input_get(ui_widget_id id, char *buf, uint32_t cap) {
    if (!buf || cap == 0) return UI_BROKER_ERR_PARAM;

    ui_broker_ops_t ops;
    uint64_t f;
    uint32_t slot;

    widget_snapshot_ops(&ops, &f);
    if (!widget_lookup(id, &slot)) {
        ui_unlock(f);
        return UI_BROKER_ERR_PARAM;
    }
    uint32_t type = g_widgets[slot].type;
    ui_unlock(f);

    if (type != UI_WIDGET_INPUT) return UI_BROKER_ERR_PARAM;
    if (!ops.input_get) return 0;
    return ops.input_get(ops.ctx, id, buf, cap);
}

/* ------------------------------------------------------------------ olay */

int ui_broker_event_push(ui_widget_id id, uint32_t ev, int32_t a, int32_t b) {
    if (ev == UI_EVENT_NONE) return UI_BROKER_ERR_PARAM;

    uint64_t f = ui_lock();
    if (g_ev_count >= UI_BROKER_EVENT_MAX) {
        ui_unlock(f);
        return UI_BROKER_ERR_FULL;
    }
    uint32_t ev_app = UI_APP_NONE;
    uint32_t slot;
    if (widget_lookup(id, &slot)) ev_app = g_widgets[slot].app;
    g_events[g_ev_tail].id = id;
    g_events[g_ev_tail].type = ev;
    g_events[g_ev_tail].a = a;
    g_events[g_ev_tail].b = b;
    g_events[g_ev_tail].app = ev_app;
    g_ev_tail = (g_ev_tail + 1u) % UI_BROKER_EVENT_MAX;
    g_ev_count++;
    ui_unlock(f);
    return UI_BROKER_OK;
}

int ui_broker_event_wait(ui_event_t *out, uint32_t timeout_ticks) {
    if (!out) return UI_BROKER_ERR_PARAM;

    uint64_t start = clock_ms();
    uint32_t stuck = 0;

    for (;;) {
        uint64_t f = ui_lock();
        if (g_ev_count > 0) {
            out->id = g_events[g_ev_head].id;
            out->type = g_events[g_ev_head].type;
            out->a = g_events[g_ev_head].a;
            out->b = g_events[g_ev_head].b;
            g_ev_head = (g_ev_head + 1u) % UI_BROKER_EVENT_MAX;
            g_ev_count--;
            ui_unlock(f);
            return UI_BROKER_OK;
        }
        ui_unlock(f);

        if (timeout_ticks == 0) return UI_BROKER_ERR_EMPTY;
        uint64_t now = clock_ms();
        if (now - start >= (uint64_t)timeout_ticks) return UI_BROKER_ERR_EMPTY;

        /* Saat ilerlemiyorsa (kesmeler kapali) sonsuza kadar donme. */
        if (now == start) {
            if (++stuck > 50000000u) return UI_BROKER_ERR_EMPTY;
        } else {
            stuck = 0;
        }
        __asm__ volatile("pause");
    }
}

uint32_t ui_broker_event_pending(void) {
    uint64_t f = ui_lock();
    uint32_t n = g_ev_count;
    ui_unlock(f);
    return n;
}

void ui_broker_event_flush(void) {
    uint64_t f = ui_lock();
    g_ev_head = g_ev_tail = g_ev_count = 0;
    ui_unlock(f);
}

/* -------------------------------------------------------------- yetenek */

int ui_broker_set_caps(uint32_t app_id, uint32_t caps) {
    if (app_id >= UI_BROKER_APP_MAX) return UI_BROKER_ERR_PARAM;
    uint64_t f = ui_lock();
    g_caps[app_id] = caps & UI_BROKER_CAP_ALL;
    g_caps_known[app_id] = 1;
    ui_unlock(f);
    return UI_BROKER_OK;
}

uint32_t ui_broker_get_caps(uint32_t app_id) {
    if (app_id >= UI_BROKER_APP_MAX) return 0;
    uint64_t f = ui_lock();
    uint32_t caps = g_caps_known[app_id] ? g_caps[app_id] : 0;
    ui_unlock(f);
    return caps;
}

int ui_broker_check_caps(uint32_t app_id, uint32_t cap) {
    if (app_id >= UI_BROKER_APP_MAX) return UI_BROKER_ERR_PARAM;
    if (cap == 0) return UI_BROKER_OK;

    uint64_t f = ui_lock();
    int ok = g_caps_known[app_id] &&
             ((g_caps[app_id] & cap) == (cap & UI_BROKER_CAP_ALL));
    ui_unlock(f);
    return ok ? UI_BROKER_OK : UI_BROKER_ERR_NOCAP;
}

int ui_broker_clear_caps(uint32_t app_id) {
    if (app_id >= UI_BROKER_APP_MAX) return UI_BROKER_ERR_PARAM;
    uint64_t f = ui_lock();
    g_caps[app_id] = 0;
    g_caps_known[app_id] = 0;
    ui_unlock(f);
    return UI_BROKER_OK;
}

/* ------------------------------------------------------------------ bind */

int ui_broker_widget_bind(ui_widget_id id, uint32_t app_id) {
    if (app_id >= UI_BROKER_APP_MAX) return UI_BROKER_ERR_PARAM;
    uint64_t f = ui_lock();
    uint32_t slot;
    if (!widget_lookup(id, &slot)) {
        ui_unlock(f);
        return UI_BROKER_ERR_PARAM;
    }
    g_widgets[slot].app = app_id;
    ui_unlock(f);
    return UI_BROKER_OK;
}

/* ---------------------------------------------------------------- upcall */

int ui_broker_set_upcall(uint32_t app_id, ui_upcall_fn fn, void *ctx) {
    if (app_id >= UI_BROKER_APP_MAX) return UI_BROKER_ERR_PARAM;
    uint64_t f = ui_lock();
    g_upcall[app_id] = fn;
    g_upcall_ctx[app_id] = fn ? ctx : 0;
    ui_unlock(f);
    return UI_BROKER_OK;
}

#define UI_UPCALL_BATCH 16u

int ui_broker_upcall_drain(uint32_t app_id, uint32_t max_events) {
    if (app_id >= UI_BROKER_APP_MAX) return UI_BROKER_ERR_PARAM;
    if (max_events == 0 || max_events > UI_UPCALL_BATCH)
        max_events = UI_UPCALL_BATCH;

    ui_upcall_fn fn;
    void *ctx;
    ui_event_node_t batch[UI_UPCALL_BATCH];
    ui_event_node_t kept[UI_BROKER_EVENT_MAX];
    uint32_t n = 0, kn = 0;

    uint64_t f = ui_lock();
    fn = g_upcall[app_id];
    ctx = g_upcall_ctx[app_id];
    if (!fn) {
        ui_unlock(f);
        return UI_BROKER_ERR_STATE;
    }
    for (uint32_t i = 0; i < g_ev_count; i++) {
        uint32_t idx = (g_ev_head + i) % UI_BROKER_EVENT_MAX;
        if (g_events[idx].app == app_id && n < max_events) {
            batch[n++] = g_events[idx];
        } else {
            kept[kn++] = g_events[idx];
        }
    }
    for (uint32_t i = 0; i < kn; i++) g_events[i] = kept[i];
    g_ev_head = 0;
    g_ev_tail = kn % UI_BROKER_EVENT_MAX;
    g_ev_count = kn;
    ui_unlock(f);

    for (uint32_t i = 0; i < n; i++) {
        ui_event_t ev = { batch[i].id, batch[i].type, batch[i].a, batch[i].b };
        int rc = fn(ctx, &ev);
        if (rc < 0) return (int)n;
    }
    return (int)n;
}

/* --------------------------------------------------- taslak syscall yuzeyi
 * S5a syscall ABI basligi commit edilince numara gecisi ve kullanici
 * bellek kopyasi (copy_in/out) oraya tasinacak; simdilik isaretciler
 * cekirdek-gorunur kabul edilir. */

static int ui_sys_cap_gate(uint32_t app_id, uint32_t cap) {
    if (app_id == UI_APP_NONE) return UI_BROKER_OK;
    return ui_broker_check_caps(app_id, cap);
}

int64_t ui_broker_syscall(uint32_t app_id, uint32_t nr,
                          uint64_t a0, uint64_t a1,
                          uint64_t a2, uint64_t a3) {
    int rc;

    switch (nr) {
    case UI_SYS_WIDGET_CREATE: {
        rc = ui_sys_cap_gate(app_id, UI_BROKER_CAP_CREATE);
        if (rc < 0) return rc;
        ui_widget_id id = UI_WIDGET_INVALID;
        rc = ui_broker_widget_create((uint32_t)a0, (ui_widget_id)a1, &id);
        if (rc < 0) return rc;
        if (app_id != UI_APP_NONE) (void)ui_broker_widget_bind(id, app_id);
        return (int64_t)id;
    }
    case UI_SYS_WIDGET_DESTROY:
        rc = ui_sys_cap_gate(app_id, UI_BROKER_CAP_DESTROY);
        if (rc < 0) return rc;
        return ui_broker_widget_destroy((ui_widget_id)a0);
    case UI_SYS_WIDGET_INFO: {
        uint32_t *out_type = (uint32_t *)(uintptr_t)a1;
        ui_widget_id *out_parent = (ui_widget_id *)(uintptr_t)a2;
        rc = ui_broker_widget_info((ui_widget_id)a0, out_type, out_parent);
        return rc;
    }
    case UI_SYS_LABEL_SET:
        rc = ui_sys_cap_gate(app_id, UI_BROKER_CAP_LABEL);
        if (rc < 0) return rc;
        return ui_broker_label_set((ui_widget_id)a0, (const char *)(uintptr_t)a1);
    case UI_SYS_BUTTON_SET:
        rc = ui_sys_cap_gate(app_id, UI_BROKER_CAP_BUTTON);
        if (rc < 0) return rc;
        return ui_broker_button_set((ui_widget_id)a0,
                                    (const char *)(uintptr_t)a1, (int)a2);
    case UI_SYS_INPUT_GET:
        rc = ui_sys_cap_gate(app_id, UI_BROKER_CAP_INPUT);
        if (rc < 0) return rc;
        return ui_broker_input_get((ui_widget_id)a0,
                                   (char *)(uintptr_t)a1, (uint32_t)a2);
    case UI_SYS_EVENT_WAIT: {
        rc = ui_sys_cap_gate(app_id, UI_BROKER_CAP_EVENTS);
        if (rc < 0) return rc;
        return ui_broker_event_wait((ui_event_t *)(uintptr_t)a0,
                                    (uint32_t)a1);
    }
    case UI_SYS_EVENT_POST:
        rc = ui_sys_cap_gate(app_id, UI_BROKER_CAP_EVENTS);
        if (rc < 0) return rc;
        return ui_broker_event_push((ui_widget_id)a0, (uint32_t)a1,
                                    (int32_t)a2, (int32_t)a3);
    default:
        return UI_BROKER_ERR_PARAM;
    }
}
