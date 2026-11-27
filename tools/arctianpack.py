#!/usr/bin/env python3
"""
Arctian Packer
1) Modulleri + boot imajini tek bir HDD disk imajina (.img) paketler.
2) El Torito no-emulation bootable ISO (.iso) uretir.

HDD yerlesimi:
  LBA 0..          : stage1 + stage2 + main_kernel (boot imaji)
  LBA 2048         : superblock
  LBA 2049         : module table
  LBA 2050         : state sector
  LBA 4096..       : modul imajlari (512B AMOD basligi + binary)

ISO yerlesimi:
  boot_iso.img     : stage1_iso + stage2 + main_kernel (BIOS 0x7C00'e yukler)
  install.bin, kernel_plus.bin, desktop.bin : modul dosyalari (ISO9660)
"""
import json
import os
import shutil
import struct
import subprocess
import sys

AMOD_MAGIC = 0x444F4D41
AMOD_ABI = 2
SB_MAGIC = 0x54435241
SB_VERSION = 1
SECTOR = 512


def cksum(data: bytes) -> int:
    s = 0x12345678
    for b in data:
        s = ((s << 1) & 0xFFFFFFFF) ^ (s >> 31) ^ b
        s &= 0xFFFFFFFF
    return s


def read(path: str) -> bytes:
    with open(path, "rb") as f:
        return f.read()


def pad(data: bytes, align: int = SECTOR) -> bytes:
    r = len(data) % align
    return data + b"\x00" * (align - r) if r else data


def elf_bss_size(path: str) -> int:
    data = read(path)
    if data[:4] != b"\x7fELF":
        return 0
    e_shoff = struct.unpack_from("<Q", data, 0x28)[0]
    e_shentsize = struct.unpack_from("<H", data, 0x3A)[0]
    e_shnum = struct.unpack_from("<H", data, 0x3C)[0]
    total = 0
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_type = struct.unpack_from("<I", data, off + 4)[0]
        sh_size = struct.unpack_from("<Q", data, off + 0x20)[0]
        if sh_type == 8:
            total += sh_size
    return total


def build_amod(name, mtype, load_addr, image, bss_size, flags=0):
    hdr = struct.pack(
        "<IHHIIIII32s16sI",
        AMOD_MAGIC, AMOD_ABI, mtype, load_addr, len(image), bss_size, 0,
        cksum(image),
        name.encode()[:31].ljust(32, b"\x00"),
        b"0.1".ljust(16, b"\x00"),
        flags,
    )
    return hdr + b"\x00" * (SECTOR - len(hdr)) + image


def build_efi_esp_img(bootx64_path: str, kernel_elf_path: str, out_img: str) -> int:
    r"""FAT32 EFI System Partition imaji uretir (\EFI\BOOT\BOOTX64.EFI + \EFI\ARCTIAN\KERNEL.ELF)."""
    for tool in ("mformat", "mmd", "mcopy"):
        if not shutil.which(tool):
            raise SystemExit(f"  HATA: UEFI ESP imaji icin '{tool}' (mtools) gerekli")

    need_bytes = os.path.getsize(bootx64_path) + os.path.getsize(kernel_elf_path) + 2 * 1024 * 1024
    # FAT32 sartnamesi >= 65525 kume (cluster) gerektirir (~33 MB+); OVMF FatDxe
    # 32 MB altindaki zorlanmis FAT32 hacimlerini reddeder.
    mb = max(36, (need_bytes + (1024 * 1024 - 1)) // (1024 * 1024))
    img_bytes = mb * 1024 * 1024

    with open(out_img, "wb") as f:
        f.truncate(img_bytes)

    def _run(args):
        r = subprocess.run(args, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        if r.returncode != 0:
            raise SystemExit(
                f"  HATA: {' '.join(args)} basarisiz (RC={r.returncode}): "
                + (r.stderr or b"").decode(errors="replace").strip()
            )

    _run(["mformat", "-i", out_img, "-F", "-v", "ARCTIAN_EFI", "::"])
    _run(["mmd", "-i", out_img, "::/EFI", "::/EFI/BOOT", "::/EFI/ARCTIAN"])
    _run(["mcopy", "-o", "-i", out_img, bootx64_path, "::/EFI/BOOT/BOOTX64.EFI"])
    _run(["mcopy", "-o", "-i", out_img, bootx64_path, "::/EFI/ARCTIAN/BOOTX64.EFI"])
    _run(["mcopy", "-o", "-i", out_img, kernel_elf_path, "::/EFI/ARCTIAN/KERNEL.ELF"])
    live_flag = out_img + ".livecd"
    with open(live_flag, "wb") as lf:
        lf.write(b"LIVE")
    _run(["mcopy", "-o", "-i", out_img, live_flag, "::/EFI/ARCTIAN/LIVECD"])
    try:
        os.remove(live_flag)
    except OSError:
        pass
    return img_bytes


def _find_optional_file(env_name: str, candidates) -> str:
    env_val = os.environ.get(env_name, "").strip()
    if env_val:
        return env_val if os.path.isfile(env_val) else ""
    for p in candidates:
        if p and os.path.isfile(p):
            return p
    return ""


def build_iso(cfg, stage2, main_kernel, module_payloads, img_bytes=b"", hd_boot=b""):
    stage1_iso = bytearray(read(cfg["boot"]["stage1_iso"]))

    payload = bytes(stage2) + main_kernel
    if len(payload) > 0x20000:
        raise SystemExit("  HATA: ISO boot payload stage1_iso 0x30000 sinirini asiyor (%d > 131072)" % len(payload))
    mag = stage1_iso.find(b"PBYT")
    if mag < 4:
        print("  UYARI: stage1_iso 'PBYT' isareti bulunamadi; ISO payload yamalanmadi")
    else:
        struct.pack_into("<I", stage1_iso, mag - 4, len(payload) & 0xFFFFFFFF)

    boot_iso = pad(bytes(stage1_iso) + payload, 2048)
    boot_sectors = len(boot_iso) // SECTOR

    out_iso = os.environ.get("ARCTIAN_OUT_ISO", cfg["output"]["iso"])
    os.makedirs(os.path.dirname(os.path.abspath(out_iso)), exist_ok=True)
    iso_root = os.path.join(
        os.path.dirname(os.path.abspath(out_iso)), f".iso_root_{os.getpid()}"
    )
    shutil.rmtree(iso_root, ignore_errors=True)
    os.makedirs(iso_root, exist_ok=True)
    with open(os.path.join(iso_root, "boot_iso.img"), "wb") as f:
        f.write(boot_iso)
    # Kurulum icin disk onyukleme imaji (stage1 + stage2 + main_kernel)
    with open(os.path.join(iso_root, "hdimg.bin"), "wb") as f:
        f.write(hd_boot)
    for fname, disk in module_payloads:
        with open(os.path.join(iso_root, fname), "wb") as f:
            f.write(disk)

    # Opsiyonel kurulum icerigi: WPE WebKit + DejaVuSans.ttf + CA sertifikalari
    # (iso9660.c 8.3 buyuk harf adlariyla dogrudan okunur)
    bundled = []
    webkit_src = _find_optional_file(
        "ARCTIAN_WEBKIT_ELF",
        ["/home/duck/webkit-build/tmp/arctian_webkit.stripped.elf"],
    )
    if webkit_src:
        shutil.copyfile(webkit_src, os.path.join(iso_root, "WEBKIT.ELF"))
        bundled.append(("WEBKIT.ELF", os.path.getsize(webkit_src)))

    dejavu_src = _find_optional_file(
        "ARCTIAN_DEJAVU_TTF",
        ["design/font/DejaVuSans.ttf"],
    )
    if dejavu_src:
        shutil.copyfile(dejavu_src, os.path.join(iso_root, "DEJAVU.TTF"))
        bundled.append(("DEJAVU.TTF", os.path.getsize(dejavu_src)))

    cacert_src = _find_optional_file(
        "ARCTIAN_CACERT_PEM",
        ["/etc/ssl/certs/ca-certificates.crt", "design/certs/cacert.pem"],
    )
    if cacert_src:
        shutil.copyfile(cacert_src, os.path.join(iso_root, "CACERT.PEM"))
        bundled.append(("CACERT.PEM", os.path.getsize(cacert_src)))

    # On-yuklu Ofis .wlf paketleri (build/wlf/ veya ARCTIAN_OFFICE_DIR)
    office_dir = os.environ.get("ARCTIAN_OFFICE_DIR", "").strip() or os.path.join("build", "wlf")
    for wlf_name in ("safak", "murekkep", "abakus", "sahne", "mercek"):
        wlf_src = os.path.join(office_dir, f"{wlf_name}.wlf")
        if os.path.isfile(wlf_src):
            iso_wlf = f"{wlf_name.upper()}.WLF"
            shutil.copyfile(wlf_src, os.path.join(iso_root, iso_wlf))
            bundled.append((iso_wlf, os.path.getsize(wlf_src)))

    # Hibrit BIOS + UEFI (Cift El Torito + ISOHybrid MBR/GPT)
    bootx64_src = _find_optional_file("ARCTIAN_UEFI_EFI", ["build/uefi/BOOTX64.EFI"])
    default_kelf = os.path.splitext(cfg["boot"]["main_kernel"])[0] + ".elf"
    kelf_src = _find_optional_file("ARCTIAN_UEFI_KERNEL_ELF", [default_kelf])

    has_uefi = bool(bootx64_src and kelf_src)
    if os.environ.get("ARCTIAN_REQUIRE_UEFI") == "1" and not has_uefi:
        raise SystemExit(
            f"  HATA: UEFI Hibrit ISO icin BOOTX64.EFI ({bootx64_src}) ve "
            f"KERNEL.ELF ({kelf_src}) gerekli"
        )

    tmp_iso = out_iso + ".part"
    if has_uefi:
        efi_img_path = os.path.join(iso_root, "efi.img")
        efi_bytes = build_efi_esp_img(bootx64_src, kelf_src, efi_img_path)
        os.makedirs(os.path.join(iso_root, "EFI", "BOOT"), exist_ok=True)
        os.makedirs(os.path.join(iso_root, "EFI", "ARCTIAN"), exist_ok=True)
        shutil.copyfile(bootx64_src, os.path.join(iso_root, "EFI", "BOOT", "BOOTX64.EFI"))
        shutil.copyfile(bootx64_src, os.path.join(iso_root, "EFI", "ARCTIAN", "BOOTX64.EFI"))
        shutil.copyfile(kelf_src, os.path.join(iso_root, "EFI", "ARCTIAN", "KERNEL.ELF"))
        with open(os.path.join(iso_root, "EFI", "ARCTIAN", "LIVECD"), "wb") as lf:
            lf.write(b"LIVE")
        # Kok ISO9660 dizinine de kopyala ki installer.c (iso9660_find) ESP'ye kurabilsin
        shutil.copyfile(bootx64_src, os.path.join(iso_root, "BOOTX64.EFI"))
        shutil.copyfile(kelf_src, os.path.join(iso_root, "KERNEL.ELF"))
        bundled.append(("BOOTX64.EFI", os.path.getsize(bootx64_src)))
        bundled.append(("KERNEL.ELF", os.path.getsize(kelf_src)))
        cmd = [
            "xorriso", "-as", "mkisofs", "-R", "-r", "-J", "-iso-level", "3",
            "-V", "ARCTIAN_OS",
            "-part_like_isohybrid",
            "-b", "boot_iso.img", "-no-emul-boot",
            "-boot-load-size", str(boot_sectors),
            "-eltorito-alt-boot",
            "-e", "efi.img", "-no-emul-boot",
            "-isohybrid-gpt-basdat",
            "-o", tmp_iso, iso_root,
        ]
    else:
        print("  UYARI: BOOTX64.EFI veya main_kernel.elf bulunamadi; yalnizca BIOS El Torito ISO uretiliyor")
        cmd = [
            "xorriso", "-as", "mkisofs", "-R", "-r", "-J", "-iso-level", "3",
            "-V", "ARCTIAN_OS",
            "-b", "boot_iso.img", "-no-emul-boot",
            "-boot-load-size", str(boot_sectors),
            "-o", tmp_iso, iso_root,
        ]

    # ISO, imaj kadar birincil ciktidir: basarisizsa paketleme HATALI sayilir
    # (eskiden sadece uyari basiliyor, `make`/build.bat RC=0 donduruyordu).
    r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if r.returncode != 0:
        raise SystemExit("  HATA: ISO uretilemedi (xorriso RC=%d): %s"
                         % (r.returncode, (r.stderr or b"").decode(errors="replace").strip()))
    if not os.path.exists(tmp_iso) or os.path.getsize(tmp_iso) < 1024:
        raise SystemExit("  HATA: ISO uretildi ama bos/kucuk: " + tmp_iso)
    os.replace(tmp_iso, out_iso)          # atomik: yarim ISO birakmaz
    shutil.rmtree(iso_root, ignore_errors=True)
    mode_str = f"Hibrit BIOS+UEFI (ESP {efi_bytes // (1024 * 1024)} MB)" if has_uefi else "BIOS El Torito"
    print(f"  ISO     : {out_iso} ({os.path.getsize(out_iso)} byte, boot {boot_sectors} sektor, {mode_str})")
    for bname, bsz in bundled:
        print(f"  Bundle  : {bname:<12} ({bsz} byte)")


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    cfg = json.load(open(os.path.join(root, "config", "layout.json"), encoding="utf-8"))

    # Nesne dizini derleme bayraklarina gore degisir (build/obj-<hash>).
    # Yerlesim dosyasindaki "build/obj/..." yollari bu onekle yeniden yazilir.
    objdir = os.environ.get("ARCTIAN_OBJ", "build/obj").replace("\\", "/")
    def _fix(v):
        if isinstance(v, str) and v.startswith("build/obj/"):
            return objdir + v[len("build/obj"):]
        return v
    cfg["boot"] = {k: _fix(v) for k, v in cfg.get("boot", {}).items()}
    for m in cfg.get("modules", []):
        for k in ("bin", "elf"):
            if k in m:
                m[k] = _fix(m[k])

    stage1 = read(cfg["boot"]["stage1"])
    stage2 = bytearray(read(cfg["boot"]["stage2"]))
    main_kernel = read(cfg["boot"]["main_kernel"])

    if len(stage1) != 512:
        print(f"  UYARI: stage1 {len(stage1)} byte (512 olmali)")

    # stage2 son 8 byte = main_kernel boyu (HDD ve ISO ortak)
    struct.pack_into("<Q", stage2, len(stage2) - 8, len(main_kernel))

    # ---------- HDD IMG ----------
    boot_image = pad(stage1 + bytes(stage2) + main_kernel)
    boot_sectors = len(boot_image) // SECTOR
    sb_lba = cfg["superblock_lba"]
    if boot_sectors > sb_lba:
        print(f"  HATA: boot imaji {boot_sectors} sektor, superblock LBA {sb_lba} ile cakisiyor!")
        sys.exit(1)

    next_lba = cfg["module_data_start_lba"]
    module_images = []
    module_entries = []
    module_payloads = []
    for m in cfg["modules"]:
        image = read(m["bin"])
        bss = elf_bss_size(m["elf"]) if m.get("elf") else 0
        load_addr = int(m["load_addr"], 16)
        flags = m.get("flags", 0)
        disk = pad(build_amod(m["name"], m["type"], load_addr, image, bss, flags))
        sectors = len(disk) // SECTOR
        module_images.append((next_lba, disk))
        module_payloads.append((m.get("iso_name", m["name"] + ".bin"), disk))
        module_entries.append(
            struct.pack(
                "<32sHHIIIIIII",
                m["name"].encode()[:31].ljust(32, b"\x00"),
                m["type"], AMOD_ABI, flags, load_addr, next_lba, sectors,
                len(image), 0, cksum(image),
            )
        )
        next_lba += sectors

    module_table = b"".join(module_entries)
    module_table_sectors = max(1, (len(module_table) + SECTOR - 1) // SECTOR)
    module_table = pad(module_table)

    # B yuvasi (cekirdek guncellemesi) tablosu 32768'de; modul bolgesi o
    # LBA'ya dokunmamali (AFS de 65536'dan baslar, superblock.h).
    SLOTB_TABLE_LBA = 32768
    if next_lba > SLOTB_TABLE_LBA:
        raise SystemExit("  HATA: modul bolgesi bitisi %d > B tablosu %d"
                         % (next_lba, SLOTB_TABLE_LBA))
    if next_lba > 0xFFFFFFFF:
        raise SystemExit("  HATA: LBA 32-bit sinirini asti: %d" % next_lba)

    total_sectors = max(next_lba + 64, int(cfg.get("min_sectors", 524288)))
    total_bytes = total_sectors * SECTOR

    sb = struct.pack(
        "<IHHIIIIIII",
        SB_MAGIC, SB_VERSION, struct.calcsize("<IHHIIIIIII"),
        0, len(module_entries), cfg["module_table_lba"], module_table_sectors,
        cfg["state_lba"], 0, 0,
    )
    sb = sb[:-4] + struct.pack("<I", cksum(sb[:-4]))

    out_img = os.environ.get("ARCTIAN_OUT_IMG", cfg["output"]["img"])
    os.makedirs(os.path.dirname(os.path.abspath(out_img)), exist_ok=True)
    tmp_img = out_img + ".part"
    with open(tmp_img, "wb") as f:
        f.truncate(total_bytes)
        f.seek(0)
        f.write(boot_image)
        f.seek(sb_lba * SECTOR)
        f.write(sb)
        f.seek(cfg["module_table_lba"] * SECTOR)
        f.write(module_table)
        for lba, disk in module_images:
            f.seek(lba * SECTOR)
            f.write(disk)
        f.flush()
        os.fsync(f.fileno())
    if os.path.getsize(tmp_img) != total_bytes:
        raise SystemExit("  HATA: imaj tam yazilamadi: " + tmp_img)
    os.replace(tmp_img, out_img)          # atomik: kesilen derleme 0 byte .img birakmaz

    print("  Arctian paketlendi")
    print(f"  Boot    : {boot_sectors} sektor ({len(boot_image)} byte)")
    print(f"  SB      : LBA {sb_lba} ({len(module_entries)} modul)")
    for m in module_entries:
        name = struct.unpack_from("<32s", m, 0)[0].split(b"\x00")[0].decode()
        lba = struct.unpack_from("<I", m, 44)[0]
        sec = struct.unpack_from("<I", m, 48)[0]
        print(f"  Modul   : {name:<12} LBA {lba} ({sec} sektor)")
    print(f"  Imaj    : {out_img} ({total_sectors} sektor, {total_bytes // 1024} KB)")

    # ---------- ISO ----------
    if not os.environ.get("ARCTIAN_SKIP_ISO"):
        build_iso(cfg, stage2, main_kernel, module_payloads, b"", boot_image)


if __name__ == "__main__":
    main()
