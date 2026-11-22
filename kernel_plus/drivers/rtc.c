#include "rtc.h"
#include "kernel.h"

#define RTC_INDEX_PORT 0x70
#define RTC_DATA_PORT  0x71

/* CMOS index/data ikilisi paylasimli: eszamanli okuyucular (kilit saati,
 * gorev cubugu, terminal, NTP) index'i birbirine ezdirip YANLIS register
 * okuyordu (yil yerine saniye -> 2025 -> gun adi kaymasi). Kilit tum
 * register islemini kapsar; kesmeler kapali tutulur (UIP bekleme kisa). */
static volatile int s_rtc_lock;

static inline uint64_t rtc_lock(void) {
    uint64_t f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    while (__sync_lock_test_and_set(&s_rtc_lock, 1))
        __asm__ volatile("pause");
    return f;
}

static inline void rtc_unlock(uint64_t f) {
    __sync_lock_release(&s_rtc_lock);
    if (f & 0x200ull) __asm__ volatile("sti" ::: "memory");
}

static uint8_t rtc_read_reg(uint8_t reg) {
    outb(RTC_INDEX_PORT, reg);
    return inb(RTC_DATA_PORT);
}

static uint8_t rtc_bcd_to_bin(uint8_t v) {
    return (uint8_t)((v & 0x0F) + ((v >> 4) * 10));
}

static uint8_t rtc_bin_to_bcd(uint8_t v) {
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

bool rtc_read_time(rtc_time_t *out) {    if (!out) return false;
    uint64_t lf = rtc_lock();

    uint8_t last_second, last_minute, last_hour, last_day, last_month, last_year;
    uint8_t status_b;
    int guard = 100000;

    while (1) {
        while ((rtc_read_reg(0x0A) & 0x80) && guard-- > 0) {
            /* wait until update-in-progress clears */
        }
        if (guard <= 0) { rtc_unlock(lf); return false; }

        last_second = rtc_read_reg(0x00);
        last_minute = rtc_read_reg(0x02);
        last_hour   = rtc_read_reg(0x04);
        last_day    = rtc_read_reg(0x07);
        last_month  = rtc_read_reg(0x08);
        last_year   = rtc_read_reg(0x09);
        status_b    = rtc_read_reg(0x0B);

        if (last_second == rtc_read_reg(0x00) &&
            last_minute == rtc_read_reg(0x02) &&
            last_hour   == rtc_read_reg(0x04) &&
            last_day    == rtc_read_reg(0x07) &&
            last_month  == rtc_read_reg(0x08) &&
            last_year   == rtc_read_reg(0x09)) {
            break;
        }
    }

    if (!(status_b & 0x04)) {
        last_second = rtc_bcd_to_bin(last_second);
        last_minute = rtc_bcd_to_bin(last_minute);
        last_hour   = rtc_bcd_to_bin(last_hour & 0x7F);
        last_day    = rtc_bcd_to_bin(last_day);
        last_month  = rtc_bcd_to_bin(last_month);
        last_year   = rtc_bcd_to_bin(last_year);
    }

    if (!(status_b & 0x02)) {
        /* 12-hour mode */
        bool pm = last_hour & 0x80;
        last_hour &= 0x7F;
        if (pm && last_hour < 12) last_hour = (uint8_t)(last_hour + 12);
        if (!pm && last_hour == 12) last_hour = 0;
    }

    out->second = last_second;
    out->minute = last_minute;
    out->hour   = last_hour;
    out->day    = last_day;
    out->month  = last_month;
    out->year   = (uint16_t)(2000 + last_year);
    rtc_unlock(lf);
    return true;
}

/* CMOS saati yazar: NTP sonucu boylece diske/CMOS'a kalici olur.
 * register B: 0x04 = binary, 0x02 = 24 saat modu. */
void rtc_set_time(const rtc_time_t *t) {
    if (!t) return;
    uint64_t lf = rtc_lock();
    uint8_t status_b = rtc_read_reg(0x0B);
    int guard = 100000;
    while ((rtc_read_reg(0x0A) & 0x80) && guard-- > 0) { }

    uint8_t sec  = t->second, min = t->minute, hour = t->hour;
    uint8_t day  = t->day, month = t->month;
    uint8_t year = (uint8_t)(t->year >= 2000 ? t->year - 2000 : t->year);
    if (!(status_b & 0x04)) {
        sec   = rtc_bin_to_bcd(sec);
        min   = rtc_bin_to_bcd(min);
        hour  = rtc_bin_to_bcd(hour);
        day   = rtc_bin_to_bcd(day);
        month = rtc_bin_to_bcd(month);
        year  = rtc_bin_to_bcd(year);
    }

    outb(RTC_INDEX_PORT, 0x00); outb(RTC_DATA_PORT, sec);
    outb(RTC_INDEX_PORT, 0x02); outb(RTC_DATA_PORT, min);
    outb(RTC_INDEX_PORT, 0x04); outb(RTC_DATA_PORT, hour);
    outb(RTC_INDEX_PORT, 0x07); outb(RTC_DATA_PORT, day);
    outb(RTC_INDEX_PORT, 0x08); outb(RTC_DATA_PORT, month);
    outb(RTC_INDEX_PORT, 0x09); outb(RTC_DATA_PORT, year);
    rtc_unlock(lf);
}

void rtc_format_time(char *buf, int buf_size) {
    rtc_time_t now;
    if (!buf || buf_size < 6) return;

    if (!rtc_read_time(&now)) {
        if (buf_size >= 6) {
            buf[0] = '?'; buf[1] = '?'; buf[2] = ':';
            buf[3] = '?'; buf[4] = '?'; buf[5] = '\0';
        }
        return;
    }

    int h = now.hour;
    int m = now.minute;
    if (h < 0) h = 0;
    if (m < 0) m = 0;

    buf[0] = (char)('0' + (h / 10) % 10);
    buf[1] = (char)('0' + (h % 10));
    buf[2] = ':';
    buf[3] = (char)('0' + (m / 10) % 10);
    buf[4] = (char)('0' + (m % 10));
    buf[5] = '\0';
}
