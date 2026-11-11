/*
 * Arctian - teshis defteri uygulamasi
 * Ayrintili sozlesme: include/arctian/diag.h
 */
#include "diag.h"
#include "serial.h"

static diag_entry_t s_items[DIAG_MAX];
static int s_count;

static void copy_str(char *dst, int cap, const char *src) {
    if (cap <= 0) return;
    int i = 0;
    if (src) {
        for (; i < cap - 1 && src[i]; i++) dst[i] = src[i];
    }
    dst[i] = '\0';
}

void diag_reset(void) {
    for (int i = 0; i < DIAG_MAX; i++) {
        s_items[i].name[0] = '\0';
        s_items[i].detail[0] = '\0';
        s_items[i].code = 0;
        s_items[i].ok = 0;
        s_items[i].seen = 0;
    }
    s_count = 0;
}

static diag_entry_t *find_or_add(const char *name) {
    for (int i = 0; i < s_count; i++) {
        const char *a = s_items[i].name;
        const char *b = name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') return &s_items[i];
    }
    if (s_count >= DIAG_MAX) return 0;
    return &s_items[s_count++];
}

void diag_set(const char *name, bool ok, uint32_t code, const char *detail) {
    if (!name || !name[0]) return;
    diag_entry_t *e = find_or_add(name);
    if (!e) return;
    copy_str(e->name, DIAG_NAME_LEN, name);
    copy_str(e->detail, DIAG_DETAIL_LEN, detail ? detail : "");
    e->code = ok ? 0 : code;
    e->ok = ok ? 1 : 0;
    e->seen = 1;
}

void diag_ok(const char *name, const char *detail) {
    diag_set(name, true, 0, detail);
}

void diag_fail(const char *name, uint32_t code, const char *detail) {
    diag_set(name, false, code, detail);
}

int diag_count(void) {
    return s_count;
}

const diag_entry_t *diag_get(int i) {
    if (i < 0 || i >= s_count) return 0;
    return &s_items[i];
}

bool diag_all_ok(void) {
    if (s_count == 0) return false;
    for (int i = 0; i < s_count; i++) {
        if (!s_items[i].ok) return false;
    }
    return true;
}

void diag_dump(void) {
    serial_puts("[diag] ---- sistem denetimi ----\n");
    for (int i = 0; i < s_count; i++) {
        const diag_entry_t *e = &s_items[i];
        serial_printf("[diag] %s: %s", e->name, e->ok ? "OK" : "HATA");
        if (!e->ok && e->code) serial_printf(" (kod=%u)", e->code);
        if (e->detail[0]) serial_printf(" - %s", e->detail);
        serial_puts("\n");
    }
    if (s_count == 0) serial_puts("[diag] kayit yok\n");
    serial_puts("[diag] -------------------------\n");
}
