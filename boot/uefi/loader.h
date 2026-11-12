/*
 * Arctian UEFI onyukleyici - minimum EFI tanimlari (gnu-efi'siz).
 *
 * Tanimlar UEFI 2.10 spec'inden elle alindi; yalnizca kullandigimiz
 * alanlar/uyeler tanimlidir. Firmware cagrilari MS ABI iledir; tum TU
 * -mabi=ms ile derlenir, bu yuzden fonksiyon noktalari dogru cagri
 * kuralini otomatik alir. EFIAPI okunabilirlik icin bos bir makrodur.
 *
 * BootServices yapisi LocateProtocol'e (index 37) kadar tam listelenir;
 * siralama bozulursa yapilandirma ofsetleri yanlis olur. Index 37'den
 * sonraki alanlar bu pakette kullanilmadigi icin tanimli degil.
 * Kritik ofsetler _Static_assert ile kenetlenmistir.
 */
#ifndef ARCTIAN_UEFI_LOADER_H
#define ARCTIAN_UEFI_LOADER_H

#include <stddef.h>   /* offsetof */
#include <stdint.h>

#define EFIAPI /* -mabi=ms varsayilan cagri kurali yeter */

typedef uint8_t   UINT8;
typedef uint16_t  UINT16;
typedef uint32_t  UINT32;
typedef uint64_t  UINT64;
typedef unsigned long      UINTN;   /* -mabi=ms + x86_64: 64 bit */
typedef long               INTN;
typedef uint16_t  CHAR16;
typedef UINTN     EFI_STATUS;
typedef void     *EFI_HANDLE;
typedef UINT64    EFI_PHYSICAL_ADDRESS;

#define EFI_SUCCESS 0u

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

/* UEFI 2.10 "EFI_TABLE_HEADER": 24 bayt. */
typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;
_Static_assert(sizeof(EFI_TABLE_HEADER) == 24, "EFI_TABLE_HEADER 24 bayt olmali");

/* EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL (UEFI 2.10, 12.4): Revision ALANI YOKTUR;
 * ilk uyeler fonksiyon isaretcileridir. Reset=0, OutputString=8. */
typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT;
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                               UINT8 ExtendedVerification);          /* 0 */
    EFI_STATUS (EFIAPI *OutputString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                      CHAR16 *String);               /* 8 */
    /* sonraki alanlar bu pakette kullanilmiyor */
};
_Static_assert(offsetof(EFI_SIMPLE_TEXT_OUTPUT, OutputString) == 8,
               "ConOut->OutputString ofset 8 olmali");

/* EFI_BOOT_SERVICES - index 0..37. Adlandirilmamis alanlar yer tutucudur.
 * Her girdi 8 bayt; Hdr 24 bayt => LocateProtocol = 24 + 37*8 = 320. */
typedef struct {
    EFI_TABLE_HEADER Hdr;                    /*  0 (24 bayt) */
    void *RaiseTPL;                    /* 00 */
    void *CoreTPL;                     /* 01 */
    void *AllocatePages;               /* 02 */
    void *FreePages;                   /* 03 */
    void *GetMemoryMap;                /* 04 */
    void *AllocatePool;                /* 05 */
    void *FreePool;                    /* 06 */
    void *CreateEvent;                 /* 07 */
    void *SetTimer;                    /* 08 */
    void *WaitForEvent;                /* 09 */
    void *SignalEvent;                 /* 10 */
    void *CloseEvent;                  /* 11 */
    void *CheckEvent;                  /* 12 */
    void *InstallProtocolInterface;    /* 13 */
    void *ReinstallProtocolInterface;  /* 14 */
    void *UninstallProtocolInterface;  /* 15 */
    void *HandleProtocol;              /* 16 */
    void *Reserved;                    /* 17 */
    void *RegisterProtocolNotify;      /* 18 */
    void *LocateHandle;                /* 19 */
    void *LocateDevicePath;            /* 20 */
    void *InstallConfigurationTable;   /* 21 */
    void *LoadImage;                   /* 22 */
    void *StartImage;                  /* 23 */
    void *Exit;                        /* 24 */
    void *UnloadImage;                 /* 25 */
    void *ExitBootServices;            /* 26 */
    void *GetNextMonotonicCount;       /* 27 */
    void *Stall;                       /* 28 */
    void *SetWatchdogTimer;            /* 29 */
    void *ConnectController;           /* 30 */
    void *DisconnectController;        /* 31 */
    void *OpenProtocol;                /* 32 */
    void *CloseProtocol;               /* 33 */
    void *OpenProtocolInformation;     /* 34 */
    void *ProtocolsPerHandle;          /* 35 */
    void *LocateHandleBuffer;          /* 36 */
    EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *Protocol,
                                        void *Registration,
                                        void **Interface); /* 37 */
} EFI_BOOT_SERVICES;
_Static_assert(offsetof(EFI_BOOT_SERVICES, LocateProtocol) == 320,
               "BootServices->LocateProtocol ofset 320 olmali");

/* EFI_SYSTEM_TABLE (UEFI 2.10, 4.2). ConOut=64, BootServices=96. */
typedef struct {
    EFI_TABLE_HEADER Hdr;                          /*  0 */
    CHAR16 *FirmwareVendor;                        /* 24 */
    UINT32 FirmwareRevision;                       /* 32 */
    UINT32 _pad0;                                  /* 36 */
    EFI_HANDLE ConsoleInHandle;                    /* 40 */
    void *ConIn;                                   /* 48 */
    EFI_HANDLE ConsoleOutHandle;                   /* 56 */
    EFI_SIMPLE_TEXT_OUTPUT *ConOut;                /* 64 */
    EFI_HANDLE StandardErrorHandle;                /* 72 */
    EFI_SIMPLE_TEXT_OUTPUT *StdErr;                /* 80 */
    void *RuntimeServices;                         /* 88 */
    EFI_BOOT_SERVICES *BootServices;               /* 96 */
    UINTN NumberOfTableEntries;                    /* 104 */
    void *ConfigurationTable;                      /* 112 */
} EFI_SYSTEM_TABLE;
_Static_assert(offsetof(EFI_SYSTEM_TABLE, ConOut) == 64,
               "SystemTable->ConOut ofset 64 olmali");
_Static_assert(offsetof(EFI_SYSTEM_TABLE, BootServices) == 96,
               "SystemTable->BootServices ofset 96 olmali");

/* --- EFI_GRAPHICS_OUTPUT_PROTOCOL (GOP) ----------------------------------
 * Tanimlar: UEFI 2.10, 12.9 Graphics Output Protocol. */
typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,  /* 0: bayt sirasi R,G,B,X */
    PixelBlueGreenRedReserved8BitPerColor,  /* 1: bayt sirasi B,G,R,X */
    PixelBitMask,                           /* 2: maske PixelInformation'da */
    PixelBltOnly,                           /* 3: cizim yuzeyi, LFB yok */
    PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    UINT32 RedMask;
    UINT32 GreenMask;
    UINT32 BlueMask;
    UINT32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    UINT32 Version;
    UINT32 HorizontalResolution;
    UINT32 VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    EFI_PIXEL_BITMASK PixelInformation;
    UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32 MaxMode;
    UINT32 CurrentMode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

/* GOP: Revision ALANI YOK. QueryMode=0, SetMode=8, Blt=16, Mode=24. */
typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *QueryMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
                                   UINT32 ModeNumber, UINTN *SizeOfInfo,
                                   EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info); /* 0 */
    void *SetMode;                            /*  8 */
    void *Blt;                                /* 16 */
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;  /* 24 */
} EFI_GRAPHICS_OUTPUT_PROTOCOL;
_Static_assert(offsetof(EFI_GRAPHICS_OUTPUT_PROTOCOL, QueryMode) == 0,
               "GOP->QueryMode ofset 0 olmali");
_Static_assert(offsetof(EFI_GRAPHICS_OUTPUT_PROTOCOL, Mode) == 24,
               "GOP->Mode ofset 24 olmali");

/* --- loader.c'den disariya acilanlar -------------------------------------*/
void serial_init(void);
void serial_puts(const char *s);
void serial_dec(UINT64 v);
void serial_hex(UINT64 v);

/* GOP mod bilgisini bootinfo sozlesmesine gore 0x7000'e yazar (gop.c). */
void gop_report(EFI_SYSTEM_TABLE *st);

/* Giris noktasi: entry.asm cagirir (loader.c). */
EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *ST);

#endif /* ARCTIAN_UEFI_LOADER_H */
