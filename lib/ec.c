/*
 * Arctian - ACPI EC tasima katmani (baslangic iskeleti)
 *
 * Bu dosya 0.9 dalga 0'da "baglantiyi koparmamak" icin yazildi: API
 * include/arctian/ec.h'de tanimli, uygulamasi S2 hattina ait. S2 gercek
 * port erisimini yazana kadar tum cagrilar "desteklenmiyor" doner.
 *
 * DURUSTLUK: bu iskelet hicbir donanim iddiasinda bulunmaz.
 */
#include "ec.h"

bool ec_present(void) {
    return false;
}

int ec_init(void) {
    return -1;
}

int ec_read_byte(uint8_t offset, uint8_t *out) {
    (void)offset;
    if (out) *out = 0;
    return -1;
}

int ec_write_byte(uint8_t offset, uint8_t value) {
    (void)offset;
    (void)value;
    return -1;
}

int ec_read_block(uint8_t offset, uint8_t *buf, uint8_t len) {
    (void)offset;
    (void)buf;
    (void)len;
    return -1;
}

int ec_battery_percent(void) {
    return -1;
}

bool ec_ac_online(void) {
    return false;
}

int ec_brightness_set(int percent) {
    (void)percent;
    return -1;
}

int ec_brightness_get(void) {
    return -1;
}
