/*
 * Arctian UEFI onyukleyici - ESP'den cekirdek dosyasi okuma (paket 2).
 *
 * Akis: HandleProtocol(ImageHandle, LoadedImage) -> DeviceHandle uzerindeki
 * SimpleFileSystem -> OpenVolume -> Open(yol, READ) -> GetInfo(EFI_FILE_INFO)
 * ile boy -> AllocatePool -> Read dongusu (Read tam istedigimizi vermezse
 * kalan icin tekrar) -> boy + ilk 8 bayt seri porta. SFS bulunamazsa
 * LocateProtocol ile sistem geneli aranir. Yedek yol: \EFI\BOOT\KERNEL.ELF.
 * Hata durumu cokmez; STATUS kodu yazilir ve devam edilir.
 *
 * Tampon EfiLoaderData havuzundan ayrilir ve SERBEST BIRAKILMAZ: cekirdek
 * baglamina tasima paket 3'un isi. Bu yuzden uefi_info.kernel_base/size
 * doldurulur.
 */
#include "loader.h"
#include "uefi_info.h"

/* EFI_LOADED_IMAGE_PROTOCOL_GUID {5b1b31a1-9562-11d2-8e3f-00a0c969723b} */
static const EFI_GUID g_loadedimage_guid = {
    0x5B1B31A1, 0x9562, 0x11D2,
    { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B }
};
/* EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID {964e5b22-6459-11d2-8e39-00a0c969723b}
 * Dikkat: Data1 = 0x964E5B22; 0x0964E5B2 DEGIL (BlockIo 0x964E5B21'dir). */
static const EFI_GUID g_sfs_guid = {
    0x964E5B22, 0x6459, 0x11D2,
    { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B }
};
/* EFI_FILE_INFO_ID {09576e92-6d3f-11d2-8e39-00a0c969723b} */
static const EFI_GUID g_fileinfo_guid = {
    0x09576E92, 0x6D3F, 0x11D2,
    { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B }
};

#define KERNEL_MAX (64u * 1024u * 1024u)  /* 64 MB ustu makul degil */

static void serial_hexmem(const UINT8 *p, UINTN n) {
    static const char d[] = "0123456789ABCDEF";
    UINTN i;
    for (i = 0; i < n; i++) {
        serial_puts(" ");
        serial_puts((const char[]){ d[p[i] >> 4], d[p[i] & 0xF], 0 });
    }
}

static const CHAR16 *g_yol_ana = L"\\EFI\\ARCTIAN\\KERNEL.ELF";
static const CHAR16 *g_yol_yedek = L"\\EFI\\BOOT\\KERNEL.ELF";

void fs_load_kernel(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *st) {
    EFI_SIMPLE_FILE_SYSTEM *sfs = 0;
    EFI_FILE_PROTOCOL *root = 0, *file = 0;
    EFI_BOOT_SERVICES *bs;
    EFI_LOADED_IMAGE *li = 0;
    EFI_STATUS s, s1, s2;
    UINT8 infobuf[256];   /* EFI_FILE_INFO temeli 80B + dosya adi */
    EFI_FILE_INFO *fi;
    UINTN infosz, got, kalan, bu;
    UINT8 *buf;

    serial_puts("ARCTIAN-UEFI-FS: ");
    if (!st || !st->BootServices) {
        serial_puts("hata BootServices=0\r\n");
        return;
    }
    bs = st->BootServices;

    /* Dogru yol: kendi yuklendigimiz aygitin (LoadedImage->DeviceHandle)
     * SFS'i. Tek bolmeli imajda LocateProtocol ayni yeri gosterir; yine de
     * Once DeviceHandle denenir, olmazsa LocateProtocol'a dusulur. */
    s1 = bs->HandleProtocol(ImageHandle, (EFI_GUID *)&g_loadedimage_guid,
                            (void **)&li);
    s2 = 0x8000000000000003ull;  /* EFI_UNSUPPORTED: adim atlandi isareti */
    if (s1 == EFI_SUCCESS && li && li->DeviceHandle)
        s2 = bs->HandleProtocol(li->DeviceHandle, (EFI_GUID *)&g_sfs_guid,
                                (void **)&sfs);
    s = s2;
    if (!sfs) {
        s = bs->LocateProtocol((EFI_GUID *)&g_sfs_guid, 0, (void **)&sfs);
        if (!sfs) {
            serial_puts("hata SFS li=");
            serial_hex(s1);
            serial_puts(" dev=");
            serial_hex(s2);
            serial_puts(" loc=");
            serial_hex(s);
            serial_puts("\r\n");
            return;
        }
    }

    /* GECICI TESHIS: arayuz sozlesmesini dogrula. */
    serial_puts("GECICI sfs=");
    serial_hex((UINT64)(uintptr_t)sfs);
    serial_puts(" +0=");
    serial_hex(((UINT64 *)sfs)[0]);
    serial_puts(" +8=");
    serial_hex(((UINT64 *)sfs)[1]);
    serial_puts("\r\n");
    s = sfs->OpenVolume(sfs, &root);
    serial_puts("GECICI ov_st=");
    serial_hex(s);
    serial_puts(" root=");
    serial_hex((UINT64)(uintptr_t)root);
    if (s == EFI_SUCCESS && root) {
        serial_puts(" r+0=");
        serial_hex(((UINT64 *)root)[0]);
        serial_puts(" r+8=");
        serial_hex(((UINT64 *)root)[1]);
    }
    serial_puts("\r\n");
    if (s != EFI_SUCCESS || !root) {
        serial_puts("hata OpenVolume st=");
        serial_hex(s);
        serial_puts("\r\n");
        return;
    }

    /* Once birincil yol; olmazsa yedek yol. Ikisinin de status'u yazilir. */
    s1 = root->Open(root, &file, (CHAR16 *)g_yol_ana, EFI_FILE_MODE_READ, 0);
    if (s1 != EFI_SUCCESS) {
        s2 = root->Open(root, &file, (CHAR16 *)g_yol_yedek,
                        EFI_FILE_MODE_READ, 0);
        if (s2 != EFI_SUCCESS) {
            serial_puts("hata Open st1=");
            serial_hex(s1);
            serial_puts(" st2=");
            serial_hex(s2);
            serial_puts("\r\n");
            root->Close(root);
            return;
        }
    }

    infosz = sizeof(infobuf);
    s = file->GetInfo(file, (EFI_GUID *)&g_fileinfo_guid, &infosz, infobuf);
    if (s != EFI_SUCCESS) {
        serial_puts("hata GetInfo st=");
        serial_hex(s);
        serial_puts("\r\n");
        file->Close(file);
        root->Close(root);
        return;
    }
    fi = (EFI_FILE_INFO *)infobuf;
    if (fi->FileSize == 0 || fi->FileSize > KERNEL_MAX) {
        serial_puts("hata boy=");
        serial_dec(fi->FileSize);
        serial_puts("\r\n");
        file->Close(file);
        root->Close(root);
        return;
    }

    s = bs->AllocatePool(2 /* EfiLoaderData */, (UINTN)fi->FileSize,
                         (void **)&buf);
    if (s != EFI_SUCCESS) {
        serial_puts("hata AllocatePool st=");
        serial_hex(s);
        serial_puts("\r\n");
        file->Close(file);
        root->Close(root);
        return;
    }

    /* Read istenen boydan az donebilir; kalan icin dongu (1 MB dilimler). */
    got = 0;
    kalan = (UINTN)fi->FileSize;
    for (;;) {
        bu = kalan > (1u << 20) ? (1u << 20) : kalan;
        s = file->Read(file, &bu, buf + got);
        if (s != EFI_SUCCESS || bu == 0)
            break;
        got += bu;
        kalan -= bu;
        if (kalan == 0)
            break;
    }
    file->Close(file);
    root->Close(root);

    if (kalan != 0) {
        serial_puts("hata okuma eksik st=");
        serial_hex(s);
        serial_dec(got);
        serial_puts("/");
        serial_dec(fi->FileSize);
        serial_puts("\r\n");
        bs->FreePool(buf);
        return;
    }

    serial_puts("ok boyut=");
    serial_dec(got);
    serial_puts(" ilk8=");
    serial_hexmem(buf, 8);
    serial_puts(" taban=");
    serial_hex((UINT64)(uintptr_t)buf);
    serial_puts("\r\n");

    arctian_uefiinfo()->kernel_base = (uint64_t)(uintptr_t)buf;
    arctian_uefiinfo()->kernel_size = (uint64_t)got;
}
