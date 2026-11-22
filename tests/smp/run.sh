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
echo "[smp] imaj (cekirdek selftestleri acik)"
# 1.0: oz-testler uretim acilisinda kosmaz; SMP kaniti icin SELFTEST=1.
make -j"$(nproc)" pack SELFTEST=1 >/dev/null

echo "[smp] trampoline blob kontrolu"
nasm -f bin -o "$AP_TRAMP" "$TRAMP_ASM"
size=$(stat -c %s "$AP_TRAMP")
echo "[smp] blob boyutu=$size bayt (sinir 3840)"
if [ "$size" -ge 3840 ]; then
    echo "[smp] FAIL: blob 0xF00 bayti asiyor"
    exit 1
fi

echo "[smp] sembol kontrolu"
# En yeni nesne dizini secilir: build/obj-<hash> onege gore siralaninca
# eski onbellek bastirilabiliyordu (ls -t ile zaman sirasi).
elf=$(ls -t build/obj-*/kernel_plus/kernel_plus.elf | head -1)
nm "$elf" | grep -E 'ap_trampoline_bin|apboot_start_all'
nm "$elf" | grep -E 'lapic_timer_calibrate|lapic_cpu_enable|lapic_ap_mask_lint'

if [ ! -f build/arctian.img ]; then
    echo "[smp] IMG YOK, atlandi"
    exit 0
fi
# Imaj bayat olabilir: kernel_plus.bin daha yeniyse uyar (test yaniltmasin).
kpb=$(ls -t build/obj-*/kernel_plus/kernel_plus.bin 2>/dev/null | head -1)
if [ -n "$kpb" ] && [ "$kpb" -nt build/arctian.img ]; then
    echo "[smp] UYARI: build/arctian.img bayat (once 'make pack' calistirin)"
fi

echo "[smp] QEMU -smp 4 (80 sn)"
timeout 80 qemu-system-x86_64 \
    -drive file=build/arctian.img,format=raw \
    -m 1024M -smp 4 -serial stdio -display none -no-reboot \
    > "$LOG" 2>&1 || true

fail=0
if grep -E 'AP online=' "$LOG"; then
    echo "[smp] OK: AP online satirlari goruldu"
else
    echo "[smp] FAIL: AP online= satiri yok"
    fail=1
fi
if grep -E '\[sched\] cpu[1-9][0-9]* gorev devraldi' "$LOG"; then
    echo "[smp] OK: AP gercek gorev calistiriyor"
else
    echo "[smp] FAIL: AP gorev devralma satiri yok (AP'ler calismiyor)"
    fail=1
fi
if grep -E 'tick testi' "$LOG"; then
    grep -E 'tick testi' "$LOG"
fi
if [ "$fail" -eq 0 ]; then
    echo "[smp] GECTI"
    exit 0
fi
echo "[smp] FAIL"
exit 1
