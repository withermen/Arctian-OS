#include "syscall.h"
#include "idt.h"
#include "serial.h"
#include "memory_map.h"
#include "paging.h"
#include "afs.h"
#include "asi.h"
#include <stdint.h>

extern void isr_stub_128(void);

/* fd tablollari eskiden tek ve ortakti: her uygulama digerinin fd'sini
 * kapatabiliyordu. Artik her pid'in kendi tablosu var; baglayici yoksa
 * (slot 0) tek tabloya dusulur. pid'den slota modolus: pid sayisi
 * FD_PROCS'u asarsa tablolar paylasilir - o kadar canli surec yok. */
#define FD_SLOTS 16
#define FD_PROCS 32
static void    *g_fds[FD_PROCS][FD_SLOTS];
static uint32_t g_fd_off[FD_PROCS][FD_SLOTS];   /* okuma ofseti, fd basiyla:
    * ofset olmazsa donguyla okuyan ring3 uygulamasi EOF hic goremez,
    * ayni baytlari sayardi */

static arctian_cap_check_fn s_cap_check = 0;
static arctian_subject_fn   s_subject_fn = 0;
static arctian_pid_fn       s_pid_fn = 0;
static arctian_ticks_fn     s_ticks_fn = 0;

void arctian_set_cap_check(arctian_cap_check_fn fn) { s_cap_check = fn; }
void arctian_set_subject_fn(arctian_subject_fn fn) { s_subject_fn = fn; }
void arctian_set_pid_fn(arctian_pid_fn fn) { s_pid_fn = fn; }
void arctian_set_ticks_fn(arctian_ticks_fn fn) { s_ticks_fn = fn; }

static int fd_tab(void) {
    if (!s_pid_fn) return 0;
    int pid = s_pid_fn();
    if (pid < 0) return 0;
    return pid % FD_PROCS;
}

static int cap_ok(uint32_t cap) {
    if (!s_cap_check) return 1;
    return s_cap_check(cap) == 0;
}

static void apply_subject(void) {
    uint32_t uid = 0, gid = 0;
    if (s_subject_fn) s_subject_fn(&uid, &gid);
    afs_set_subject((int)uid, (int)gid);
}

void syscall_init(void) {
    idt_set_gate(0x80, (uint64_t)isr_stub_128, 0x08, IDT_USER_GATE);
}

/* Ring3 tampon denetimi. Sanal aralik tek basina yetmez: aralikta haritasiz
 * adres ya da kernel eslemesi olabilir; kernel boyle bir tamponu kendisi
 * yurutunce ya kendi cokertir (haritasiz) ya da kernel bellegini
 * kullaniciya sizdirir (eslemeli). Her 4K sayfanin gercekten kullanci
 * sayfasi (U biti) ve haritali olusu sayfalar tek tek yoklanir. */
static int user_range_w(const void *p, uint32_t len, int write) {
    uint64_t a = (uint64_t)(uintptr_t)p;
    uint64_t end = a + (uint64_t)len;
    if (end < a) return 0;
    if (a < (uint64_t)VMM_USER_BASE || end > 0x0000800000000000ull) return 0;
    vmm_space_t sp = vmm_current();
    for (uint64_t pg = a & ~0xFFFull; pg < end; pg += 0x1000ull)
        if (!vmm_probe_user(sp, pg, write)) return 0;
    return 1;
}

static int user_range(const void *p, uint32_t len) {
    return user_range_w(p, len, 0);
}

/* Kullaniciya ait NUL ile biten dize: diziler sayfa sonunu asabilir;
 * her yeni sayfaya girerken esleme yoklanir, 64 KB'den uzun dize redd. */
static int user_cstr(const char *s) {
    uint64_t a = (uint64_t)(uintptr_t)s;
    if (a < (uint64_t)VMM_USER_BASE || a >= 0x0000800000000000ull) return 0;
    vmm_space_t sp = vmm_current();
    if (!vmm_probe_user(sp, a, 0)) return 0;
    uint64_t p = a;
    for (uint32_t n = 0; n < 65536u; n++, p++) {
        if ((p & 0xFFFu) == 0 && !vmm_probe_user(sp, p, 0)) return 0;
        if (*(const char *)(uintptr_t)p == 0) return 1;
    }
    return 0;
}

void syscall_dispatch(registers_t *regs) {
    uint64_t num = regs->rax;
    uint64_t a1 = regs->rdi;
    uint64_t a2 = regs->rsi;
    uint64_t a3 = regs->rdx;
    int64_t ret = -1;

    apply_subject();

    switch (num) {
        case SYS_WRITE: {
            const char *buf = (const char *)(uintptr_t)a2;
            uint32_t len = (uint32_t)a3;
            if (!user_range(buf, len)) { ret = -2; break; }
            for (uint32_t i = 0; i < len; i++) serial_putc(buf[i]);
            ret = (int64_t)len;
            break;
        }
        case SYS_PUTCHAR:
            serial_putc((char)a1);
            ret = 0;
            break;
        case SYS_GETPID:
            ret = s_pid_fn ? (int64_t)s_pid_fn() : 1;
            break;
        case SYS_TICKS:
            ret = s_ticks_fn ? (int64_t)s_ticks_fn() : 0;
            break;
        case SYS_OPEN: {
            const char *path = (const char *)(uintptr_t)a1;
            if (!cap_ok(CAP_FS)) { ret = -1; break; }
            if (!user_cstr(path)) { ret = -2; break; }
            void *h = afs_find_handle(path);
            if (!h) h = afs_create_handle(path);
            if (!h) { ret = -2; break; }
            void **tab = g_fds[fd_tab()];
            uint32_t *off = g_fd_off[fd_tab()];
            int slot = -1;
            for (int i = 0; i < FD_SLOTS; i++) { if (!tab[i]) { slot = i; break; } }
            if (slot < 0) { ret = -3; break; }
            tab[slot] = h;
            off[slot] = 0;
            ret = slot + 3;
            break;
        }
        case SYS_READ: {
            int fd = (int)a1;
            void *buf = (void *)(uintptr_t)a2;
            uint32_t len = (uint32_t)a3;
            if (!cap_ok(CAP_FS)) { ret = -1; break; }
            if (fd < 3 || fd >= 3 + FD_SLOTS) { ret = -1; break; }
            void **tab = g_fds[fd_tab()];
            uint32_t *off = g_fd_off[fd_tab()];
            if (!tab[fd - 3]) { ret = -1; break; }
            /* cekirdek tampona YAZACAK: sayfanin yazilabilir da olmasi sart */
            if (!user_range_w(buf, len, 1)) { ret = -2; break; }
            ret = (int64_t)afs_read_at(tab[fd - 3], buf, len, off[fd - 3]);
            if (ret > 0) off[fd - 3] += (uint32_t)ret;
            break;
        }
        case SYS_CLOSE: {
            int fd = (int)a1;
            if (!cap_ok(CAP_FS)) { ret = -1; break; }
            if (fd < 3 || fd >= 3 + FD_SLOTS) { ret = -1; break; }
            void **tab = g_fds[fd_tab()];
            if (!tab[fd - 3]) { ret = -1; break; }   /* baska surecin fd'si
                                                     * degil, bizim tabloda
                                                     * yok: gecersiz */
            tab[fd - 3] = 0;
            g_fd_off[fd_tab()][fd - 3] = 0;
            ret = 0;
            break;
        }
        case SYS_EXIT:
            serial_puts("[syscall] exit\n");
            ret = 0;
            break;
        default:
            ret = -1;
            break;
    }
    regs->rax = (uint64_t)ret;
}
