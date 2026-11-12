/*
 * Arctian UEFI onyukleyici - paket 1.
 *
 * Akis: COM1'i kur, ConOut'a tek satir yaz, GOP mod bilgisini bootinfo
 * sozlesmesine gore 0x7000'e yaz, COM1'e ARCTIAN-UEFI-OK bas, firmware'e
 * don. Kernel yukleme ve ExitBootServices paket 2'nin isi.
 *
 * Not: 0x7000 BIOS yolundan miras alinan sabit adres; QEMU+OVMF'de boot
 * servisi donemine kadar bos varsayiliyor (paket 2'de AllocatePages ile
 * ayirmayi konusuruz). COM1 dogrudan porttan yazilir; test isareti ConOut
 * surucusune bagimli olmasin.
 */
#include "loader.h"
#include "bootinfo.h"

/* --- COM1 (0x3F8) dogrudan seri cikis ------------------------------------*/
#define COM1 0x3F8u

static inline void outb(UINT16 port, UINT8 val) {
    __asm__ __volatile__("outb %b0, %w1" : : "a"(val), "Nd"(port));
}

static inline UINT8 inb(UINT16 port) {
    UINT8 v;
    __asm__ __volatile__("inb %w1, %b0" : "=a"(v) : "Nd"(port));
    return v;
}

void serial_init(void) {
    outb(COM1 + 1, 0x00);  /* UART kesmeleri kapali */
    outb(COM1 + 3, 0x80);  /* DLAB ac */
    outb(COM1 + 0, 0x01);  /* bolumen 1 -> 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);  /* 8N1 */
    outb(COM1 + 2, 0xC7);  /* FIFO ac, tetik degeri 14 */
    outb(COM1 + 4, 0x0B);  /* DTR | RTS | OUT2 */
}

void serial_puts(const char *s) {
    for (; *s; s++) {
        UINTN guard = 1u << 20;  /* UART yoksa sonsuz bekleme olmasin */
        while (!(inb(COM1 + 5) & 0x20) && --guard)
            ;
        outb(COM1, (UINT8)*s);
    }
}

void serial_dec(UINT64 v) {
    char buf[21];
    int i = 0;
    if (v == 0) {
        serial_puts("0");
        return;
    }
    while (v) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i--) {
        char c[2] = { buf[i], 0 };
        serial_puts(c);
    }
}

void serial_hex(UINT64 v) {
    static const char digits[] = "0123456789ABCDEF";
    char out[19];
    int i, start = 0;
    out[0] = '0';
    out[1] = 'x';
    for (i = 0; i < 16; i++)
        out[2 + i] = digits[(v >> (60 - 4 * i)) & 0xF];
    out[18] = 0;
    while (out[start + 2] == '0' && start < 15)
        start++;
    serial_puts(out + start);
}

/* --- GOP -> bootinfo (0x7000) ---------------------------------------------
 * GOP klavuzu: UEFI 2.10 "Graphics Output Protocol" (9042a9de-...-516a). */
static const EFI_GUID g_gop_guid = {
    0x9042A9DE, 0x23DC, 0x4A38,
    { 0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A }
};

void gop_report(EFI_SYSTEM_TABLE *st) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
    arctian_bootinfo_t *bi;
    UINT32 *w;
    UINTN i;
    EFI_STATUS s;

    if (!st || !st->BootServices) {
        serial_puts("ARCTIAN-UEFI: GOP yok (BootServices 0)\r\n");
        return;
    }
    s = st->BootServices->LocateProtocol((EFI_GUID *)&g_gop_guid, 0,
                                         (void **)&gop);
    if (s != EFI_SUCCESS || !gop || !gop->Mode || !gop->Mode->Info) {
        serial_puts("ARCTIAN-UEFI: GOP yok (st=0x");
        serial_hex(s);
        serial_puts(")\r\n");
        return;
    }
    mode = gop->Mode;
    info = mode->Info;

    /* Yapinin tamamini (padding + _reserved dahil) sifirla; cop birakma. */
    bi = arctian_bootinfo();
    w = (UINT32 *)bi;
    for (i = 0; i < ARCTIAN_BOOTINFO_SIZE / 4; i++)
        w[i] = 0;

    bi->magic       = ARCTIAN_BOOTINFO_MAGIC;
    bi->fb_phys     = (UINT32)mode->FrameBufferBase;
    bi->pitch       = (UINT16)(info->PixelsPerScanLine * 4u);
    bi->width       = (UINT16)info->HorizontalResolution;
    bi->height      = (UINT16)info->VerticalResolution;
    bi->bpp         = 32;  /* iki GOP duzeni de 8/8/8/8; BltOnly zaten eleniyor */
    bi->struct_size = sizeof(arctian_bootinfo_t);
    bi->source      = ARCTIAN_BOOT_SRC_UEFI;
    bi->flags       = ARCTIAN_BOOT_FLAG_LFB_VALID | ARCTIAN_BOOT_FLAG_UEFI;
    bi->fb_size     = (UINT32)mode->FrameBufferSize;

    /* Maske sozlesmesi bootinfo.h/VBE ile ayni: piksel sozcugunun
     * little-endian 32-bit degeri icinde bit konumu. */
    switch (info->PixelFormat) {
    case PixelRedGreenBlueReserved8BitPerColor: /* bayt: R,G,B,X */
        bi->format = ARCTIAN_PIXFMT_RGBX8;
        bi->red_mask      = 0x000000FFu;
        bi->green_mask    = 0x0000FF00u;
        bi->blue_mask     = 0x00FF0000u;
        bi->reserved_mask = 0xFF000000u;
        break;
    case PixelBlueGreenRedReserved8BitPerColor: /* bayt: B,G,R,X (VBE gibi) */
        bi->format = ARCTIAN_PIXFMT_BGRX8;
        bi->red_mask      = 0x00FF0000u;
        bi->green_mask    = 0x0000FF00u;
        bi->blue_mask     = 0x000000FFu;
        bi->reserved_mask = 0xFF000000u;
        break;
    case PixelBitMask:
        bi->format = ARCTIAN_PIXFMT_UNKNOWN;
        bi->red_mask      = info->PixelInformation.RedMask;
        bi->green_mask    = info->PixelInformation.GreenMask;
        bi->blue_mask     = info->PixelInformation.BlueMask;
        bi->reserved_mask = info->PixelInformation.ReservedMask;
        break;
    default: /* PixelBltOnly / PixelFormatMax: LFB gecerli degil */
        bi->format = ARCTIAN_PIXFMT_UNKNOWN;
        break;
    }

    serial_puts("ARCTIAN-UEFI: GOP ");
    serial_dec(info->HorizontalResolution);
    serial_puts("x");
    serial_dec(info->VerticalResolution);
    serial_puts(" pitch=");
    serial_hex(bi->pitch);
    serial_puts(" fb=");
    serial_hex(mode->FrameBufferBase);
    serial_puts(" size=");
    serial_hex(mode->FrameBufferSize);
    serial_puts(" fmt=");
    serial_dec(bi->format);
    serial_puts("\r\n");
}

/* --- firmware girisi ------------------------------------------------------*/
EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *ST) {
    static CHAR16 banner[] = L"Arctian UEFI\n";

    (void)ImageHandle;
    serial_init();
    serial_puts("ARCTIAN-UEFI: basladi\r\n");

    if (ST && ST->ConOut && ST->ConOut->OutputString)
        ST->ConOut->OutputString(ST->ConOut, banner);

    gop_report(ST);

    serial_puts("ARCTIAN-UEFI-OK\r\n");  /* test isareti: ConOut'tan bagimsiz */
    return EFI_SUCCESS;
}
