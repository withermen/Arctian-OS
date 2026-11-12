#!/bin/sh
# Arctian UEFI testi (S1): make efi -> FAT ESP -> OVMF/QEMU -> COM1 isareti.
# Beklenen: logda "ARCTIAN-UEFI-OK". 30 sn QEMU; OVMF yoksa 1 doner (skipped
# ayirt etmek icin PANIC satiri basilir).
set -e
cd "$(dirname "$0")/../.."

ROOT=$(pwd)
ESP=build/uefi-esp
LOG=build/uefi-test.log

# Brief geregi betik kendi derlemesini yapsin (sicak build'ta no-op).
if ! timeout 300 make efi >build/uefi-build.log 2>&1; then
    echo "FAIL: make efi cikisi hatasi; build/uefi-build.log:"
    tail -n 10 build/uefi-build.log
    exit 1
fi

mkdir -p "$ESP/EFI/BOOT"
cp build/uefi/BOOTX64.EFI "$ESP/EFI/BOOT/BOOTX64.EFI"

# OVMF secimi: tek dosya -bios tercih; yoksa CODE+VARS pflash ikilisi.
BIOS_ARGS=""
if [ -f /usr/share/ovmf/OVMF.fd ]; then
    BIOS_ARGS="-bios /usr/share/ovmf/OVMF.fd"
elif [ -f /usr/share/OVMF/OVMF_CODE_4M.fd ] && [ -f /usr/share/OVMF/OVMF_VARS_4M.fd ]; then
    cp /usr/share/OVMF/OVMF_CODE_4M.fd build/ovmf-code.fd
    cp /usr/share/OVMF/OVMF_VARS_4M.fd build/ovmf-vars.fd
    BIOS_ARGS="-drive if=pflash,format=raw,readonly=on,file=$ROOT/build/ovmf-code.fd \
-drive if=pflash,format=raw,file=$ROOT/build/ovmf-vars.fd"
else
    echo "ARCTIAN-UEFI-PANIC: OVMF bulunamadi (/usr/share/ovmf/OVMF.fd veya /usr/share/OVMF/OVMF_CODE_4M.fd)" | tee "$LOG"
    exit 1
fi

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "ARCTIAN-UEFI-PANIC: qemu-system-x86_64 yok" | tee "$LOG"
    exit 1
fi

timeout 30 qemu-system-x86_64 -machine q35 -m 1G -net none -nographic \
    $BIOS_ARGS \
    -drive format=raw,file=fat:rw:$ESP \
    >"$LOG" 2>&1 || true

if grep -q "ARCTIAN-UEFI-OK" "$LOG"; then
    echo "PASS: ARCTIAN-UEFI-OK goruldu ($LOG)"
    exit 0
fi

echo "FAIL: ARCTIAN-UEFI-OK yok; log:"
cat "$LOG"
exit 1
