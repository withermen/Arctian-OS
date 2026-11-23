#!/bin/sh
# Arctian UEFI testi (S1): make efi -> mtools ile MBR+FAT32 ESP imaji ->
# OVMF/QEMU -> COM1 isareti. Beklenen: logda "ARCTIAN-UEFI-OK".
#
# Notlar:
# - Link bayraklari (--subsystem 10 --image-base=0x400000) Makefile'daki
#   efi kuralinda; betik duz `make efi` kullanir.
# - dosfstools WSL'de yok; imaj dd+sfdisk+mformat (mtools) ile kurulur.
# - QEMU fat:rw surucusu yerine gercek imaj: OVMF partition+FAT yolunu
#   gercek MBR uzerinden gecer, test sahte surucuye bagimli olmaz.
set -e
cd "$(dirname "$0")/../.."

ROOT=$(pwd)
IMG=build/uefi-esp.img
LOG=build/uefi-test.log

# Derleme: brief geregi betik kendi derlemesini yapsin (sicak build hizli).
rm -f build/uefi/BOOTX64.EFI
if ! timeout 300 make efi >build/uefi-build.log 2>&1; then
    echo "FAIL: make efi cikisi hatasi; build/uefi-build.log:"
    tail -n 10 build/uefi-build.log
    exit 1
fi

# Paket 3: varsa main_kernel.elf'i ESP'ye KERNEL.ELF olarak yerlestir.
KELF=$(ls -t build/obj-*/main_kernel/main_kernel.elf 2>/dev/null | head -n 1 || true)

# FAT imaji: MBR + tek FAT32 bolmesi. dosfstools yerine mtools (WSL'de hazır).
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count=32 status=none
printf 'label: dos\nunit: sectors\nstart=2048, type=c, bootable\n' |
    sfdisk --quiet --wipe never "$IMG"
MDEV=$IMG@@1048576
mformat -i "$MDEV" :: >/dev/null
mmd -i "$MDEV" ::/EFI ::/EFI/BOOT ::/EFI/ARCTIAN
mcopy -o -i "$MDEV" build/uefi/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
if [ -n "$KELF" ] && [ -f "$KELF" ]; then
    mcopy -o -i "$MDEV" "$KELF" ::/EFI/ARCTIAN/KERNEL.ELF
fi

# OVMF secimi: tek dosya -bios tercih; yoksa CODE+VARS pflash ikilisi.
BIOS_ARGS="-bios /usr/share/ovmf/OVMF.fd"
if [ ! -f /usr/share/ovmf/OVMF.fd ]; then
    if [ -f /usr/share/OVMF/OVMF_CODE_4M.fd ] && [ -f /usr/share/OVMF/OVMF_VARS_4M.fd ]; then
        cp /usr/share/OVMF/OVMF_CODE_4M.fd build/ovmf-code.fd
        cp /usr/share/OVMF/OVMF_VARS_4M.fd build/ovmf-vars.fd
        BIOS_ARGS="-drive if=pflash,format=raw,readonly=on,file=$ROOT/build/ovmf-code.fd \
-drive if=pflash,format=raw,file=$ROOT/build/ovmf-vars.fd"
    else
        echo "ARCTIAN-UEFI-PANIC: OVMF bulunamadi (/usr/share/ovmf/OVMF.fd veya /usr/share/OVMF/OVMF_CODE_4M.fd)" | tee "$LOG"
        exit 1
    fi
fi

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "ARCTIAN-UEFI-PANIC: qemu-system-x86_64 yok" | tee "$LOG"
    exit 1
fi

# shellcheck disable=SC2086  # BIOS_ARGS bilincli olarak bozulmus (birden çok arg)
timeout 30 qemu-system-x86_64 -machine q35 -m 1G -net none -nographic \
    $BIOS_ARGS \
    -drive format=raw,file=$IMG \
    >"$LOG" 2>&1 || true

# ANSI kodlarini sipurup temiz logu cikar (FAIL'de o katilir).
CLEAN=build/uefi-test.clean.log
tr -d '\r' < "$LOG" |
    sed 's/\x1b\[[0-9;?]*[A-Za-z]//g' > "$CLEAN"

if grep -aq "ARCTIAN-UEFI-OK" "$CLEAN"; then
    echo "PASS: ARCTIAN-UEFI-OK goruldu ($CLEAN)"
    grep -a "ARCTIAN-UEFI" "$CLEAN"
    exit 0
fi

echo "FAIL: ARCTIAN-UEFI-OK yok; log:"
cat "$CLEAN"
exit 1
