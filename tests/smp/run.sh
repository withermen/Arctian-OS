#!/usr/bin/env bash
# S6 SMP test senaryosu: derleme + statik kontrol + (imaj varsa) QEMU.
# Calistirma: bash tests/smp/run.sh   (worktree kokunden)
set -e
set -u

cd "$(dirname "$0")/../.."

AP_TRAMP=build/ap_trampoline.bin
TRAMP_ASM=kernel_plus/system/ap_trampoline.asm
LOG=/tmp/smp_qemu.log

echo "[smp] kernel_plus derleniyor"
make -j"$(nproc)" kernel_plus

echo "[smp] trampoline blob kontrolu"
nasm -f bin -o "$AP_TRAMP" "$TRAMP_ASM"
size=$(stat -c %s "$AP_TRAMP")
echo "[smp] blob boyutu=$size bayt (sinir 3840)"
if [ "$size" -ge 3840 ]; then
    echo "[smp] FAIL: blob 0xF00 bayti asiyor"
    exit 1
fi

echo "[smp] sembol kontrolu"
elf=$(ls build/obj-*/kernel_plus/kernel_plus.elf | head -1)
nm "$elf" | grep -E 'ap_trampoline_bin|apboot_start_all'

if [ ! -f build/arctian.img ]; then
    echo "[smp] IMG YOK, atlandi"
    exit 0
fi

echo "[smp] QEMU -smp 4 (40 sn)"
timeout 40 qemu-system-x86_64 \
    -drive file=build/arctian.img,format=raw \
    -m 512M -smp 4 -serial stdio -display none -no-reboot \
    > "$LOG" 2>&1 || true

if grep -E 'AP online=' "$LOG"; then
    echo "[smp] GECTI: AP online satiri goruldu"
    exit 0
fi

echo "[smp] FAIL: AP online= satiri yok"
exit 1
