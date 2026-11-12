/*
 * Arctian - ACPI Embedded Controller (EC) erisimi
 *
 * Dizustu pil, sicaklik, fan ve parlaklik degerleri genelde EC uzerinden
 * okunur. EC, ACPI ad alanindaki `_REG`/EC alanlari ile erisilir; 0.9'da
 * AML yorumlayicisi olmadigi icin dogrudan port (0x62/0x66) erisimi
 * kullanilir ve makineye ozel ofsetler "best effort" kabul edilir.
 *
 * Uygulayan: S2 (lib/ec.c)
 * Kullanan : S2 pil/termal, S3 parlaklik/pil arayuzu (dolayli)
 *
 * DURUSTLUK: EC alan yerlesimleri makineden makineye degisir; bu API'nin
 * her makinede calistigi iddia edilemez. Okunamayan veya akla yatkin
 * araliga girmeyen alanlar icin fonksiyon false doner ve cikti
 * parametresine DOKUNMAZ. Cagiran taraf "bilinmiyor" gostermeli,
 * uydurmamali.
 */
#ifndef ARCTIAN_EC_H
#define ARCTIAN_EC_H

#include <stdint.h>
#include <stdbool.h>

/* ACPI EC standart register'lari (port tabanli) */
#define EC_DATA_PORT        0x62
#define EC_CMD_PORT         0x66
#define EC_STATUS_OBF       0x01    /* output buffer full */
#define EC_STATUS_IBF       0x02    /* input buffer full */
#define EC_CMD_READ         0x80
#define EC_CMD_WRITE        0x81
#define EC_CMD_BURST_ENABLE 0x82
#define EC_CMD_BURST_DISABLE 0x83

/* EC bulundu mu? (port yoklamasi; ACPI ECDT/AML gerekmez) */
bool ec_present(void);

/* EC'yi kesfet ve hazirla. Basarili: 0, yok/hata: <0. */
int  ec_init(void);

/* Tek bayt oku/yaz. Basarili: 0, zaman asimi/hata: <0. */
int  ec_read_byte(uint8_t offset, uint8_t *out);
int  ec_write_byte(uint8_t offset, uint8_t value);

/* Ardisik blok oku (bayt bayt; burst modu kullanilmaz). */
int  ec_read_block(uint8_t offset, uint8_t *buf, uint8_t len);

/* Pil yuzdesi (0-100). Basarili: true ve *pct yazilir.
 * Okunamaz/saglamasiz: false ve *pct degismez. */
bool ec_battery_percent(int *pct);

/* AC adaptor takili mi. Basarili: true ve *on yazilir; bilinmiyorsa false. */
bool ec_ac_online(bool *on);

/* Pil takili mi (gecerli yuzde okunabiliyorsa true). */
bool ec_battery_present(void);

/* Parlaklik: 0-100 arasi hedef yuzde; desteklenmiyorsa <0. */
int  ec_brightness_set(int percent);
int  ec_brightness_get(void);

#endif /* ARCTIAN_EC_H */
