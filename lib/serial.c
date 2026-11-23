/**
 * Arctian COM1 seri debug cikisi (0x3F8)
 */
#include "serial.h"
#include "kernel.h"
#include <stdarg.h>

void serial_init(void) {
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x80);
    outb(0x3F8 + 0, 0x03);
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03);
    outb(0x3F8 + 2, 0xC7);
    outb(0x3F8 + 4, 0x0B);
}

static volatile uint32_t s_serial_lock;

static uint64_t serial_lock_acquire(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    while (__atomic_exchange_n(&s_serial_lock, 1u, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause" ::: "memory");
    return flags;
}

static void serial_lock_release(uint64_t flags) {
    __atomic_store_n(&s_serial_lock, 0u, __ATOMIC_RELEASE);
    if (flags & 0x200ull) __asm__ volatile("sti" ::: "memory");
}

static int tx_empty(void) { return inb(0x3F8 + 5) & 0x20; }

static void serial_putc_raw(char c) {
    while (!tx_empty()) { }
    outb(0x3F8, (uint8_t)c);
}

static void serial_puts_raw(const char *s) {
    while (*s) serial_putc_raw(*s++);
}

void serial_putc(char c) {
    uint64_t f = serial_lock_acquire();
    serial_putc_raw(c);
    serial_lock_release(f);
}

void serial_puts(const char *s) {
    uint64_t f = serial_lock_acquire();
    serial_puts_raw(s);
    serial_lock_release(f);
}

static void sput_uint(unsigned long v, int base, int upper, int width) {
    char buf[32];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;
    if (v == 0) buf[i++] = '0';
    while (v && i < 20) { buf[i++] = digits[v % (unsigned long)base]; v /= (unsigned long)base; }
    /* Genislik dolgusu tamponu tasiyabilirdi: %99999d cagrisi yigin
     * uzerine yazardi. Genislik tampona sigan en buyuk degerle sinirlandi. */
    if (width > 30) width = 30;
    while (i < width) buf[i++] = '0';
    while (i--) serial_putc_raw(buf[i]);
}

void serial_printf(const char *fmt, ...) {
    uint64_t f = serial_lock_acquire();
    va_list ap;
    va_start(ap, fmt);
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { serial_putc_raw(*p); continue; }
        p++;
        if (!*p) { serial_putc_raw('%'); break; }
        int width = 0;
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }
        /* l/ll gorunup geciliyordu ama arguman yine int okunuyordu;
         * %lld sonrasi butun argumanlar kayiyordu. */
        int lng = 0;
        while (*p == 'l') { lng = 1; p++; }
        switch (*p) {
            case 's': { const char *s = va_arg(ap, const char *); serial_puts_raw(s ? s : "(null)"); break; }
            case 'c': serial_putc_raw((char)va_arg(ap, int)); break;
            case 'd': {
                if (lng) {
                    long v = va_arg(ap, long);
                    if (v < 0) { serial_putc_raw('-'); v = -v; }
                    sput_uint((unsigned long)v, 10, 0, width);
                } else {
                    int v = va_arg(ap, int);
                    if (v < 0) { serial_putc_raw('-'); v = -v; }
                    sput_uint((unsigned long)(unsigned)v, 10, 0, width);
                }
                break;
            }
            case 'u':
                if (lng) sput_uint(va_arg(ap, unsigned long), 10, 0, width);
                else     sput_uint((unsigned long)va_arg(ap, unsigned), 10, 0, width);
                break;
            case 'x':
                if (lng) sput_uint(va_arg(ap, unsigned long), 16, 0, width);
                else     sput_uint((unsigned long)va_arg(ap, unsigned), 16, 0, width);
                break;
            case 'X':
                if (lng) sput_uint(va_arg(ap, unsigned long), 16, 1, width);
                else     sput_uint((unsigned long)va_arg(ap, unsigned), 16, 1, width);
                break;
            case '%': serial_putc_raw('%'); break;
            default: serial_putc_raw('%'); if (*p) serial_putc_raw(*p); break;
        }
    }
    va_end(ap);
    serial_lock_release(f);
}
