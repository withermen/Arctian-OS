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

static const CHAR16 *const g_kernel_paths[] = {
    L"\\EFI\\ARCTIAN\\KERNEL.ELF",
    L"\\EFI\\BOOT\\KERNEL.ELF",
    L"\\ARCTIAN\\KERNEL.ELF",
    L"\\KERNEL.ELF"
};
static const char *const g_kernel_path_names[] = {
    "\\EFI\\ARCTIAN\\KERNEL.ELF",
    "\\EFI\\BOOT\\KERNEL.ELF",
    "\\ARCTIAN\\KERNEL.ELF",
    "\\KERNEL.ELF"
};

extern int g_uefi_livecd;

static int try_open_on_sfs(EFI_SIMPLE_FILE_SYSTEM *sfs,
                           EFI_FILE_PROTOCOL **out_root,
                           EFI_FILE_PROTOCOL **out_file,
                           int *out_idx) {
    EFI_FILE_PROTOCOL *root = 0, *file = 0;
    EFI_STATUS s = sfs->OpenVolume(sfs, &root);
    if (s != EFI_SUCCESS || !root) return -1;

    UINTN npaths = sizeof(g_kernel_paths) / sizeof(g_kernel_paths[0]);
    for (UINTN i = 0; i < npaths; i++) {
        s = root->Open(root, &file, (CHAR16 *)g_kernel_paths[i],
                       EFI_FILE_MODE_READ, 0);
        if (s == EFI_SUCCESS && file) {
            EFI_FILE_PROTOCOL *live_marker = 0;
            if (root->Open(root, &live_marker, L"\\EFI\\ARCTIAN\\LIVECD",
                           EFI_FILE_MODE_READ, 0) == EFI_SUCCESS && live_marker) {
                g_uefi_livecd = 1;
                live_marker->Close(live_marker);
            }
            *out_root = root;
            *out_file = file;
            *out_idx = (int)i;
            return 0;
        }
    }
    root->Close(root);
    return -2;
}

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
    int path_idx = -1;

    serial_puts("ARCTIAN-UEFI-FS: ");
    if (!st || !st->BootServices) {
        serial_puts("hata BootServices=0\r\n");
        return;
    }
    bs = st->BootServices;

    /* 1) Once yuklendigimiz aygitin (LoadedImage->DeviceHandle) SFS hacmini dene. */
    s1 = bs->HandleProtocol(ImageHandle, (EFI_GUID *)&g_loadedimage_guid,
                            (void **)&li);
    s2 = 0x8000000000000003ull;
    if (s1 == EFI_SUCCESS && li && li->DeviceHandle) {
        s2 = bs->HandleProtocol(li->DeviceHandle, (EFI_GUID *)&g_sfs_guid,
                                (void **)&sfs);
        if (s2 == EFI_SUCCESS && sfs) {
            if (try_open_on_sfs(sfs, &root, &file, &path_idx) != 0) {
                root = 0;
                file = 0;
            }
        }
    }

    /* 2) Bulunamazsa sistemdeki tum SimpleFileSystem hacimlerini tara. */
    if (!file && bs->LocateHandleBuffer) {
        typedef EFI_STATUS (EFIAPI *locate_hb_fn)(UINT32, EFI_GUID *, void *, UINTN *, EFI_HANDLE **);
        locate_hb_fn lhb = (locate_hb_fn)bs->LocateHandleBuffer;
        EFI_HANDLE *handles = 0;
        UINTN nhandles = 0;
        if (lhb(2 /* ByProtocol */, (EFI_GUID *)&g_sfs_guid, 0, &nhandles, &handles) == EFI_SUCCESS && handles) {
            for (UINTN h = 0; h < nhandles && !file; h++) {
                EFI_SIMPLE_FILE_SYSTEM *hsfs = 0;
                if (bs->HandleProtocol(handles[h], (EFI_GUID *)&g_sfs_guid, (void **)&hsfs) == EFI_SUCCESS && hsfs) {
                    if (try_open_on_sfs(hsfs, &root, &file, &path_idx) != 0) {
                        root = 0;
                        file = 0;
                    }
                }
            }
            if (bs->FreePool) bs->FreePool(handles);
        }
    }

    /* 3) Son care: LocateProtocol tekil SFS dene. */
    if (!file && bs->LocateProtocol) {
        EFI_SIMPLE_FILE_SYSTEM *lsfs = 0;
        if (bs->LocateProtocol((EFI_GUID *)&g_sfs_guid, 0, (void **)&lsfs) == EFI_SUCCESS && lsfs) {
            if (try_open_on_sfs(lsfs, &root, &file, &path_idx) != 0) {
                root = 0;
                file = 0;
            }
        }
    }

    if (!file) {
        serial_puts("KERNEL.ELF bulunamadi (yedek: firmware'e donus) li=");
        serial_hex(s1);
        serial_puts(" dev=");
        serial_hex(s2);
        serial_puts("\r\n");
        return;
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

    serial_puts("ok yol=");
    serial_puts(path_idx >= 0 ? g_kernel_path_names[path_idx] : "?");
    serial_puts(" boyut=");
    serial_dec(got);
    serial_puts(" ilk8=");
    serial_hexmem(buf, 8);
    serial_puts(" taban=");
    serial_hex((UINT64)(uintptr_t)buf);
    serial_puts("\r\n");

    arctian_uefiinfo()->kernel_base = (uint64_t)(uintptr_t)buf;
    arctian_uefiinfo()->kernel_size = (uint64_t)got;
}
