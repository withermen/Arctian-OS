/*
 * Arctian - ACPI EC tasima katmani
 *
 * ACPI AML yorumlayicisi olmadigi icin EC'ye dogrudan port uzerinden
 * (0x62 veri / 0x66 komut) erisilir. Tum islemler sinirli sayida
 * yoklamayla zaman asimina ugrar; EC yoksa veya yanit vermezse
 * fonksiyonlar false/-1 doner ve cikti parametrelerine yazilmaz.
 *
 * PIL / AC / TERMAL / KAPAK
 * -------------------------
 * EC RAM yerlesimi standart degildir: hangi ofsetin pil yuzdesi,
 * hangi bitin kapak oldugu makinenin DSDT'sinde tanimlidir. AML
 * ayristiricisi olmadigi icin burada birkac yaygin yerlesim adayi
 * denenir ve ancak alanlar birlikte tutarli ise (yuzde 0..100,
 * durum bitleri 0..3, sarj varken AC cevrimici, sicaklik fiziksel
 * aralikta) kabul edilir. Boyle bir yerlesim bulunamazsa degerler
 * "bilinmiyor" sayilir. Bu bir kesif degil, muhafazakar bir tahmindir;
 * bu makinede dogrulanmamis olabilir.
 *
 * DURUSTLUK: "her makinede calisir" iddiasi yoktur.
 */
#include "ec.h"
#include "kernel.h"
#include "serial.h"

/* Port yoklamasi icin ust sinir. Her tur io_wait (~1 us) icerir;
 * 100000 tur yaklasik 0.1 s eder, EC yoksa sistem kilitlenmez. */
#define EC_TIMEOUT 100000

/* Pil durum baytlari (ACPI _BST bitleri ile ayni kabul edilir):
 * bit0 = desarj, bit1 = sarj, bit2 = kritik. */
#define EC_BST_DISCHARGING 0x01
#define EC_BST_CHARGING    0x02

/*
 * Makineye ozel EC alan yerlesimi adayi.
 * 0 ofseti "alan yok" anlamina gelir; EC offset 0 hicbir alan icin
 * kullanilmaz.
 */
typedef struct {
    const char *name;
    uint8_t status_off;     /* _BST benzeri durum bayti */
    uint8_t pct_off;        /* pil yuzdesi */
    uint8_t ac_off;         /* AC durumu bayti (0 = alan yok) */
    uint8_t ac_bit;
    uint8_t ac_inverted;    /* 1 = bit 0 iken AC takili */
    uint8_t thermal_off;    /* termal bayt (0 = alan yok) */
    int8_t  thermal_offset; /* ham deger + offset = santigrat */
    uint8_t thermal_kelvin; /* 1 = ham deger Kelvin */
    uint8_t lid_off;        /* kapak bayti (0 = alan yok) */
    uint8_t lid_bit;
    uint8_t lid_closed_high;/* 1 = bit 1 iken kapak kapali */
} ec_layout_t;

/*
 * Aday yerlesimler. Bunlar toplulukta gorulen EC haritalarina
 * benzetilmis tahminlerdir; belirli bir dizustunde dogrulanmadi.
 * Dogrulama siki tutuldugu icin uymayan makinede "bilinmiyor" cikar.
 */
static const ec_layout_t s_layouts[] = {
    /* a: 0x30'lu blok (bazi dizustuleri) */
    { "a", 0x30, 0x31, 0x3B, 0, 0, 0x78, 0, 0, 0x3C, 0, 1 },
    /* b: 0x50'li blok */
    { "b", 0x50, 0x51, 0x52, 0, 0, 0x70, 0, 0, 0x53, 0, 1 },
    /* c: 0xB0'li blok */
    { "c", 0xB0, 0xB1, 0xB2, 0, 0, 0xB3, 0, 0, 0xB4, 0, 1 },
    /* d: 0xE0'li blok */
    { "d", 0xE0, 0xE1, 0xE2, 0, 0, 0xE3, 0, 0, 0xE4, 0, 1 },
};

#define EC_LAYOUT_COUNT ((int)(sizeof(s_layouts) / sizeof(s_layouts[0])))

/* -1: henuz yoklanmadi, 0: EC yok, 1: EC var */
static int s_probe = -1;
/* -1: secilmedi, -2: uygun yerlesim yok, >=0: s_layouts indeksi */
static int s_layout = -1;

/* ------------------------------------------------------------------ */
/* Port erisimi                                                       */
/* ------------------------------------------------------------------ */

static int ec_wait_ibf_clear(void) {
    for (int i = 0; i < EC_TIMEOUT; i++) {
        if (!(inb(EC_CMD_PORT) & EC_STATUS_IBF)) return 0;
        io_wait();
    }
    return -1;
}

static int ec_wait_obf_set(void) {
    for (int i = 0; i < EC_TIMEOUT; i++) {
        if (inb(EC_CMD_PORT) & EC_STATUS_OBF) return 0;
        io_wait();
    }
    return -1;
}

/* Onceki islemden kalan bayt varsa oku ve at (aksi halde bir sonraki
 * okuma eski veriyi dondurur). */
static void ec_drain_obf(void) {
    if (inb(EC_CMD_PORT) & EC_STATUS_OBF) (void)inb(EC_DATA_PORT);
}

/* Ham okuma: ec_present() yoklamasi bu yolu kullanir, kendine
 * cagri yapmaz. */
static int ec_raw_read(uint8_t offset, uint8_t *out) {
    if (!out) return -1;
    if (ec_wait_ibf_clear()) return -2;
    ec_drain_obf();
    outb(EC_CMD_PORT, EC_CMD_READ);
    if (ec_wait_ibf_clear()) return -2;
    outb(EC_DATA_PORT, offset);
    if (ec_wait_ibf_clear()) return -2;
    if (ec_wait_obf_set()) return -3;
    *out = inb(EC_DATA_PORT);
    return 0;
}

static int ec_raw_write(uint8_t offset, uint8_t value) {
    if (ec_wait_ibf_clear()) return -2;
    ec_drain_obf();
    outb(EC_CMD_PORT, EC_CMD_WRITE);
    if (ec_wait_ibf_clear()) return -2;
    outb(EC_DATA_PORT, offset);
    if (ec_wait_ibf_clear()) return -2;
    outb(EC_DATA_PORT, value);
    if (ec_wait_ibf_clear()) return -2;
    return 0;
}

/* Durum portu bosuna mi bakiyor? Yoklama: 0xFF veya anlamsiz ust
 * bitler EC olmadigini gosterir; ardindan tek bayt okuma denenir. */
static bool ec_probe(void) {
    uint8_t st = inb(EC_CMD_PORT);
    if (st == 0xFF) return false;
    if (st & (uint8_t)~0x0Fu) return false;
    uint8_t v = 0;
    if (ec_raw_read(0x00, &v) != 0) return false;
    return true;
}

int ec_init(void) {
    s_probe = ec_probe() ? 1 : 0;
    s_layout = -1;
    return s_probe ? 0 : -1;
}

bool ec_present(void) {
    if (s_probe < 0) (void)ec_init();
    return s_probe == 1;
}

int ec_read_byte(uint8_t offset, uint8_t *out) {
    if (!ec_present()) return -1;
    return ec_raw_read(offset, out);
}

int ec_write_byte(uint8_t offset, uint8_t value) {
    if (!ec_present()) return -1;
    return ec_raw_write(offset, value);
}

int ec_read_block(uint8_t offset, uint8_t *buf, uint8_t len) {
    if (!buf) return -1;
    if (!ec_present()) return -1;
    for (uint8_t i = 0; i < len; i++) {
        int rc = ec_raw_read((uint8_t)(offset + i), &buf[i]);
        if (rc != 0) return rc;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Alan yerlesimi secimi                                              */
/* ------------------------------------------------------------------ */

static int ec_read_layout(uint8_t off, uint8_t *v) {
    if (!off) return -1;
    return ec_read_byte(off, v);
}

static bool ec_layout_check(const ec_layout_t *l) {
    uint8_t pct = 0, status = 0;

    if (!l->pct_off || !l->status_off) return false;
    if (ec_read_layout(l->pct_off, &pct) != 0) return false;
    if (ec_read_layout(l->status_off, &status) != 0) return false;

    /* Yuzde 0..100, durum yalnizca alt dort bit. */
    if (pct > 100) return false;
    if (status & (uint8_t)~0x0Fu) return false;

    bool charging = (status & EC_BST_CHARGING) != 0;
    bool discharging = (status & EC_BST_DISCHARGING) != 0;
    /* Ayni anda sarj ve desarj olamaz. */
    if (charging && discharging) return false;

    if (l->ac_off) {
        uint8_t ac = 0;
        if (ec_read_layout(l->ac_off, &ac) != 0) return false;
        uint8_t mask = (uint8_t)(1u << l->ac_bit);
        /* AC bayti yalnizca kendi bitini tasimali; paylasimli bayt
         * varsayimi yanlis olabilir, bu yuzden muhafazakar reddedilir. */
        if (ac & (uint8_t)~mask) return false;
        bool online = ((ac & mask) != 0);
        if (l->ac_inverted) online = !online;
        /* Fiziksel tutarlilik: sarj varken AC takili olmali, desarj
         * varken AC takili olmamali. */
        if (charging && !online) return false;
        if (discharging && online) return false;
    }

    if (l->thermal_off) {
        uint8_t t = 0;
        if (ec_read_layout(l->thermal_off, &t) != 0) return false;
        if (l->thermal_kelvin) {
            if (t < 220) return false;  /* uint8: ust sinir 255 K */
        } else {
            int c = (int)t + l->thermal_offset;
            if (c < 0 || c > 125) return false;
        }
    }

    if (l->lid_off) {
        uint8_t lid = 0;
        if (ec_read_layout(l->lid_off, &lid) != 0) return false;
        uint8_t mask = (uint8_t)(1u << l->lid_bit);
        if (lid & (uint8_t)~mask) return false;
    }

    return true;
}

static int ec_layout_pick(void) {
    if (s_layout != -1) return s_layout;
    s_layout = -2;
    if (!ec_present()) return s_layout;
    for (int i = 0; i < EC_LAYOUT_COUNT; i++) {
        if (ec_layout_check(&s_layouts[i])) {
            s_layout = i;
            break;
        }
    }
    return s_layout;
}

/* ------------------------------------------------------------------ */
/* Genel API                                                          */
/* ------------------------------------------------------------------ */

bool ec_battery_percent(int *pct) {
    if (!pct) return false;
    int li = ec_layout_pick();
    if (li < 0) return false;
    uint8_t v = 0;
    if (ec_read_byte(s_layouts[li].pct_off, &v) != 0) return false;
    if (v > 100) return false;
    *pct = (int)v;
    return true;
}

bool ec_ac_online(bool *on) {
    if (!on) return false;
    int li = ec_layout_pick();
    if (li < 0) return false;
    const ec_layout_t *l = &s_layouts[li];

    if (l->ac_off) {
        uint8_t ac = 0;
        if (ec_read_byte(l->ac_off, &ac) != 0) return false;
        uint8_t mask = (uint8_t)(1u << l->ac_bit);
        if (ac & (uint8_t)~mask) return false;
        bool online = ((ac & mask) != 0);
        if (l->ac_inverted) online = !online;
        *on = online;
        return true;
    }

    /* Ayri AC bayti yoksa sarj durumundan cikarim yapilir. */
    uint8_t status = 0;
    if (ec_read_byte(l->status_off, &status) != 0) return false;
    if (status & EC_BST_CHARGING) { *on = true; return true; }
    if (status & EC_BST_DISCHARGING) { *on = false; return true; }
    return false;
}

bool ec_battery_present(void) {
    int pct = 0;
    return ec_battery_percent(&pct);
}

/* ------------------------------------------------------------------ */
/* Parlaklik (yerlesim makineye ozel; desteklenmiyor)                 */
/* ------------------------------------------------------------------ */

int ec_brightness_set(int percent) {
    (void)percent;
    return -1;
}

int ec_brightness_get(void) {
    return -1;
}
