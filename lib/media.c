/**
 * Arctian ortam soyutlama: ATA disk (superblock) veya ATAPI CD (ISO9660).
 * Boot medyasi stage1 tarafindan 0x7100'e yazilir: 0/1=ATA, 2=CD.
 */
#include "media.h"
#include "storage.h"
#include "atapi.h"
#include "iso9660.h"
#include "sbio.h"
#include "amod.h"
#include "kernel.h"
#include "slot.h"

#define BOOT_MEDIA_ADDR 0x7100

static int is_cd = 0;
static int s_slot_b = 0;

/* ---- A/B yuva durumu (boot tarafi) ----
 * slot.c masaustu ASI'siyle calisir; boot yolu ASI'siz oldugu icin ayni
 * kayit burada ham sektor uzerinden okunur/yazilir. */
static uint32_t slot_crc32(const uint8_t *p, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1u)));
    }
    return c ^ 0xFFFFFFFFu;
}

static int slot_state_read(slot_state_t *out) {
    uint8_t buf[512];
    if (storage_read(STATE_LBA, 1, buf) != 0) return -1;
    slot_state_t s;
    for (uint32_t i = 0; i < sizeof(s); i++) ((uint8_t *)&s)[i] = buf[i];
    if (s.magic != SLOT_MAGIC || s.version != SLOT_VERSION) return -2;
    if (s.size != (uint16_t)sizeof(s) || s.active > 1) return -2;
    if (s.crc != slot_crc32((const uint8_t *)&s, (uint32_t)sizeof(s) - 4u)) return -2;
    *out = s;
    return 0;
}

static int slot_state_write(const slot_state_t *in) {
    slot_state_t s = *in;
    s.magic = SLOT_MAGIC;
    s.version = SLOT_VERSION;
    s.size = (uint16_t)sizeof(s);
    s.crc = slot_crc32((uint8_t *)&s, (uint32_t)sizeof(s) - 4u);
    uint8_t buf[512];
    for (int i = 0; i < 512; i++) buf[i] = 0;
    for (uint32_t i = 0; i < sizeof(s); i++) buf[i] = ((const uint8_t *)&s)[i];
    return storage_write(STATE_LBA, 1, buf);
}

/* Erken acilis bekcesi: masaustune hic ulasamayan yuva iki denemede
 * birakilir. Masaustu acildiginda boot_ok 1 olur (slot_mark_boot_ok). */
int media_boot_attempt(void) {
    if (is_cd) return 0;
    slot_state_t s;
    if (slot_state_read(&s) != 0) return 0;   /* taze disk: A zaten saglam */
    if (s.boot_ok) return 0;
    if (s.tries >= 2) {
        s.active = (uint8_t)(1 - s.active);
        s.pending = 0xFF;
        s.boot_ok = 1;
        s.tries = 0;
        if (slot_state_write(&s) != 0) return -1;
        s_slot_b = (s.active == SLOT_B) ? 1 : 0;
        return 2;                              /* onceki yuvaya donuldu */
    }
    s.tries++;
    if (slot_state_write(&s) != 0) return -1;
    return 1;                                  /* bu yuva denenecek */
}

int media_slot(void) { return s_slot_b; }

int media_init(void) {
    uint8_t src = *(volatile uint8_t *)BOOT_MEDIA_ADDR;
#ifdef ARCTIAN_BLK_SELFTEST
    { char m[40]; int n=0; const char *p="[mk] boot_media="; while(*p) m[n++]=*p++; m[n++]=(char)('0'+(src%10)); m[n++]='\n'; m[n]=0; extern void serial_puts(const char*); serial_puts(m); }
#endif
    is_cd = (src == 2);
    if (is_cd) {
        s_slot_b = 0;
        if (atapi_init() != 0) return -1;
        if (iso9660_init() != 0) return -1;
        return 0;
    }
    if (storage_init() != 0) return -1;
    {   /* Hangi yuva etkin? Guncellenmis cekirdek B tablosundan yuklenir. */
        slot_state_t s;
        s_slot_b = (slot_state_read(&s) == 0 && s.active == SLOT_B) ? 1 : 0;
    }
    return 0;
}

int media_is_cd(void) { return is_cd; }

int media_read(uint32_t lba, uint32_t count, void *buf) {
    if (!is_cd) return storage_read(lba, count, buf);

    uint8_t *out = (uint8_t *)buf;
    uint32_t off = lba * 512u;
    uint32_t bytes = count * 512u;
    static uint8_t sec[2048];
    while (bytes) {
        uint32_t s = off / 2048u;
        uint32_t so = off % 2048u;
        uint32_t n = 2048u - so;
        if (n > bytes) n = bytes;
        int ok = 0;
        for (int attempt = 0; attempt < 4 && !ok; attempt++) {
            if (atapi_read(s, 1, sec) == 0) ok = 1;
        }
        if (!ok) return -1;
        for (uint32_t i = 0; i < n; i++) out[i] = sec[so + i];
        out += n;
        off += n;
        bytes -= n;
    }
    return 0;
}

int media_installed(void) {
    if (is_cd) return 0; /* canli ortam */
    superblock_t sb;
    if (sb_load(&sb) != 0 || sb.magic != SUPERBLOCK_MAGIC) return 0;
    return (sb.flags & SB_FLAG_INSTALLED) ? 1 : 0;
}

int media_set_installed(void) {
    if (is_cd) return -1;
    superblock_t sb;
    if (sb_load(&sb) != 0 || sb.magic != SUPERBLOCK_MAGIC) return -1;
    sb.flags |= SB_FLAG_INSTALLED;
    return sb_store(&sb);
}

/* ISO9660 8.3 isim siniri: modul adi -> ISO dosya tabani */
static const char *iso_name_for(const char *name) {
    if (strcmp(name, "kernel_plus") == 0) return "KPLUS";
    if (strcmp(name, "install") == 0) return "INSTALL";
    if (strcmp(name, "desktop") == 0) return "DESKTOP";
    return name;
}

int media_find_module(const char *name, module_entry_t *out) {
    if (is_cd) {
        uint32_t lba2048 = 0, size = 0;
        if (iso9660_find(iso_name_for(name), &lba2048, &size) != 0) return -1;
        uint32_t lba512 = lba2048 * 4u;

        uint8_t hdr[512];
        if (media_read(lba512, 1, hdr) != 0) return -1;
        const amod_header_t *h = (const amod_header_t *)hdr;
        if (h->magic != AMOD_MAGIC) return -1;

        for (int i = 0; i < 31; i++) {
            out->name[i] = name[i];
            if (!name[i]) break;
        }
        out->name[31] = '\0';
        out->type = h->type;
        out->abi_version = h->abi_version;
        out->flags = h->flags;
        out->load_addr = h->load_addr;
        out->image_lba = lba512;
        out->image_sectors = 0;
        out->image_size = h->image_size;
        out->entry_off = h->entry_off;
        out->checksum = h->checksum;
        return 0;
    }

    /* B yuvasi: guncellemede yazilmis modul tablosu tek sektordur
     * (magic + sayi + en fazla 7 girdi). Bozuksa A tablosuna dusulur;
     * sistem bozuk B yuzunden hic acilmamazlik etmesin. */
    if (s_slot_b) {
        uint8_t sec[512];
        if (storage_read(SLOTB_TABLE_LBA, 1, sec) == 0) {
            uint32_t magic = 0, cnt = 0;
            for (int b = 0; b < 4; b++) { magic |= (uint32_t)sec[b] << (8 * b);
                                          cnt   |= (uint32_t)sec[4 + b] << (8 * b); }
            if (magic == SLOTB_TABLE_MAGIC && cnt <= 7) {
                for (uint32_t i = 0; i < cnt; i++) {
                    module_entry_t e;
                    const uint8_t *g = sec + 8 + i * 64u;
                    for (uint32_t b = 0; b < sizeof(e); b++) ((uint8_t *)&e)[b] = g[b];
                    e.name[31] = 0;
                    if (strcmp(e.name, name) == 0) { *out = e; return 0; }
                }
            }
        }
    }

    superblock_t sb;
    if (sb_load(&sb) != 0 || sb.magic != SUPERBLOCK_MAGIC) return -1;
    module_entry_t table[MAX_MODULES];
    if (storage_read(sb.module_tab_lba, sb.module_tab_sectors, table) != 0) return -1;
    for (uint32_t i = 0; i < sb.module_count; i++) {
        if (strcmp(table[i].name, name) == 0) {
            *out = table[i];
            return 0;
        }
    }
    return -1;
}
