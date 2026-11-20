#!/usr/bin/env bash
# B1/B2 .wlf izolasyon testi: derleme + statik kontrol + (imaj varsa) QEMU.
# Kanit: uygulama kodu ring 3'te, kendi CR3'unde calisir; seri hatti
# dogrudan uygulamanin int 0x80 SYS_WRITE'i ile yazilir ve cikis kodu 0'dir.
# Calistirma: bash tests/isolation/run.sh   (worktree kokunden)
set -e
set -u

cd "$(dirname "$0")/../.."

LOG=/tmp/isolation_qemu.log

echo "[izolasyon] kernel_plus + masaustu derleniyor"
make -j"$(nproc)" kernel_plus desktop >/dev/null

echo "[izolasyon] sembol kontrolu"
elf=$(ls -t build/obj-*/kernel_plus/kernel_plus.elf | head -1)
nm "$elf" | grep -E ' (enter_user_arg|wlf_user_run|wlf_exit_hook|wlf_user_start)$'
# Yurutucu cekirdege bagli mi (zayif kanca yerine gercek sembol)?
nm "$elf" | grep -q ' T wlf_exit_hook' || { echo "[izolasyon] FAIL: exit kancasi yok"; exit 1; }

echo "[izolasyon] masaustu izole yolu kullaniyor mu"
grep -q 'g_asi->wlf->run_image' desktop/wlf.c || {
    echo "[izolasyon] FAIL: desktop/wlf.c izole yurutucuye baglanmamis";
    exit 1;
}

if [ ! -f build/arctian.img ]; then
    echo "[izolasyon] IMG YOK, QEMU atlandi"
    exit 0
fi
kpb=$(ls -t build/obj-*/kernel_plus/kernel_plus.bin 2>/dev/null | head -1)
if [ -n "$kpb" ] && [ "$kpb" -nt build/arctian.img ]; then
    echo "[izolasyon] UYARI: build/arctian.img bayat (once 'make pack' calistirin)"
fi

echo "[izolasyon] QEMU -smp 4 (90 sn)"
timeout 90 qemu-system-x86_64 \
    -drive file=build/arctian.img,format=raw \
    -m 1024M -smp 4 -serial stdio -display none -no-reboot \
    > "$LOG" 2>&1 || true

fail=0
if grep -q 'izole wlf ring3 ok' "$LOG"; then
    echo "[izolasyon] OK: ring 3 uygulama ciktisi seri hatta goruldu"
else
    echo "[izolasyon] FAIL: ring 3 uygulama ciktisi yok"
    fail=1
fi
if grep -q '\[wlf\] izolasyon selftest GECTI' "$LOG"; then
    echo "[izolasyon] OK: yukleme + ring 3 + exit kancasi + cikis kodu 0"
else
    echo "[izolasyon] FAIL: selftest GECTI satiri yok"
    fail=1
fi
if grep -q 'jumping to desktop' "$LOG"; then
    echo "[izolasyon] OK: sistem izole uygulamadan sonra masaustune devam etti"
else
    echo "[izolasyon] UYARI: masaustu satiri yok"
fi
if [ "$fail" -eq 0 ]; then
    echo "[izolasyon] GECTI"
    exit 0
fi
echo "[izolasyon] FAIL"
exit 1
