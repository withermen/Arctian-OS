# Derleme ve çalıştırma

## Gerekenler

Derleme Linux araç zinciri ister (freestanding `gcc`, `ld`/`objcopy`, `nasm`, `xorriso`).
Windows'ta desteklenen yol **WSL üzerinden Debian**'dır.

> **Önemli (WSL/9P notu):** Windows diskindeki (`/mnt/c/...`) klasörde doğrudan derleme yapmak
> yüzlerce nesne dosyasında 9P kilitlenmesine (D-state) yol açabilir. Bu nedenle `build.bat` ve
> geliştirici derlemeleri WSL içindeki yerel bir ext4 kopyasına (`$HOME/arctian-build`) rsync
> yaparak derler ve üretilen imajları geri kopyalar.

```sh
sudo apt update
sudo apt install -y build-essential nasm binutils python3 xorriso mtools file rsync
sudo apt install -y qemu-system-x86 ovmf       # QEMU ve UEFI testi için (isteğe bağlı)
```

| Araç | Ne için |
|---|---|
| `gcc` (freestanding) | çekirdek ve modül C kodu; `-nostdlib` ile bağlanır |
| `nasm` | `boot/*.asm` önyükleyici aşamaları ve `isr.asm` / `ring3_user.asm` |
| `ld`, `objcopy` | modül ELF'leri → düz `.bin`; `--wrap=lv_obj_send_event` |
| `python3` | paketleyici, imzacı, font/duvar kağıdı/uygulama üreticileri |
| `xorriso`, `mtools` | Hibrit BIOS+UEFI Çift El Torito ISOHybrid ISO ve FAT32 ESP imajı |
| `qemu-system-x86_64` | BIOS/UEFI çalıştırma ve kurulum testi (isteğe bağlı) |

Üçüncü parti bağımlılık gerekmez: LVGL ve mbedTLS depoya **vendored** edilmiştir
([third_party/README.md](../third_party/README.md)).

## Windows: tek adım

```bat
build.bat
```

Betik `tools/build-release.sh` aracılığıyla WSL Debian ortamını doğrular, depoyu yerel ext4
dizinine (`$HOME/arctian-build`) senkronize eder, `make -j8 pack` çalıştırır ve çıkan
`build\arctian.iso` / `build\arctian.img` dosyalarını tazelik denetimiyle geri kopyalar.

## Linux / WSL: doğrudan make

```sh
make            # tüm modüller + imaj + ISO (varsayılan hedef: all -> pack)
make efi        # UEFI önyükleyici imajı (build/BOOTX64.EFI)
make qemu-iso   # ISO'dan önyükleme (CD-ROM)
make qemu       # disk imajından önyükleme
make debug      # kurulum + hesap ekranlarını atlayan geliştirme imajı (arctian-debug.iso)
make clean      # build/ silinir
```

Çıktılar:

```
build/arctian.img       ham disk imajı (BIOS'tan doğrudan önyüklenebilir)
build/arctian.iso       Hibrit BIOS+UEFI Çift El Torito ISOHybrid ISO (USB / CD / kalıp)
build/uefi/BOOTX64.EFI  UEFI x86-64 önyükleyici uygulaması (`make efi`)
build/obj-<hash>/       nesne dosyaları (bayrak kümesinin md5 ön eki)
```

> **İsteğe bağlı ISO paket varlıkları:** `tools/arctianpack.py`, `ARCTIAN_WEBKIT_ELF`
> (`WEBKIT.ELF`), `ARCTIAN_DEJAVU_TTF` (`DEJAVU.TTF`) ve `ARCTIAN_CACERT_PEM` (`CACERT.PEM`)
> ortam değişkenleri veya varsayılan yolları bulunursa bunları da ISO'ya ekler.

Nesne dizini derleme bayraklarına göre adlandırılır (`FLAGS_HASH`): `DEBUG=1` ile `DEBUG=0`
aynı anda bozulmadan durur, karışık nesnelerden imaj üretilmez.

## Derleme bayrakları ve test hedefleri

| Değişken | Etkisi |
|---|---|
| `DEBUG=1` | açılış logları + otomatik kullanıcı (`ARCTIAN_DEBUG_BOOT`, `ARCTIAN_AUTOUSER`) |
| `SELFTEST=1` | çekirdek `sched`/`proc`/`caps`/`afs`/`lab` + SMP öz-testleri |
| `WFL_SELFTEST=1` | B1 ring-3 izolasyon öz-testi |
| `T06_TEST=1` | 0.6 öz-testleri imaja girer |
| `T07_TEST=1` | 0.7 güvenlik/imza öz-testleri imaja girer |
| `NET_TEST=1` | açılışta DNS/TCP/TLS ağ sınaması |
| `EXTRA_CFLAGS=...` | ek derleme bayrakları |

```sh
make pack DEBUG=1 SELFTEST=1     # çekirdek + SMP öz-test imajı
make pack DEBUG=1 T07_TEST=1     # imza/kapı öz-test imajı
make -j8 DEBUG=0                 # sürüm imajı
make test-uefi                   # OVMF altında UEFI önyükleme testi
make test-smp                    # QEMU -smp 4 çok çekirdek testi
bash tests/isolation/run.sh      # B1 ring-3 izolasyon testi
```

Sürüm numarası `version.json`'dan okunur ve `VERSION_CFLAGS` ile gömülür; komut satırından
`EXTRA_CFLAGS` verilse bile sürüm `0.0`'a düşmez.

## C + C++20 POSIX Çapraz Araç Zinciri (`sdk/toolchain/`)

Ring-3 ELF64 POSIX ve C++20 uygulamaları geliştirmek için Alpine v3.21 pure-musl (musl 1.2.5)
ve `libstdc++` (14.2) statik PIE sysroot'u sudo gerektirmeden kurulabilir:

```sh
bash sdk/toolchain/build-toolchain.sh    # $HOME/arctian-toolchain altına kurar
```

Derleme sarmalayıcıları ve CMake/pkg-config araçları:

```sh
sdk/toolchain/arctian-cc  -O2 program.c   -o program.elf
sdk/toolchain/arctian-cxx -O2 program.cpp -o program.elf
# CMake / pkg-config (ör. sdk/webkit/ WPE WebKit derlemeleri):
#   sdk/toolchain/arctian.cmake
#   sdk/toolchain/arctian-pkg-config
#   sdk/toolchain/setup-webkit-deps.sh
```

Doğrulama paketini derlemek için:

```sh
sdk/toolchain/arctian-cxx -O2 tests/posix/test_suite.cpp -o test_suite.elf
sdk/toolchain/arctian-cc  -O2 tests/posix/exec_helper.c  -o exec_helper.elf
```

Üretilen ELF64 ikilileri Arctian terminalinde `elfrun C:/yol/program.elf` komutuyla çalıştırılır.

## Üretileni doğrulama

```sh
file build/arctian.iso                 # ISO-9660 boot image, El Torito
isoinfo -d -i build/arctian.iso        # CD001, boot catalog 0100, BOOTABLE
ls -l build/arctian.img build/arctian.iso
```

ISO üretilemezse paketleyici derlemeyi **dur­durur** (yarım/0 baytlık çıktı bırakmaz).

## QEMU'da denemek

```sh
qemu-system-x86_64 -cdrom build/arctian.iso -boot d -m 1024M -vga std -serial stdio
qemu-system-x86_64 -drive file=build/arctian.img,format=raw -m 1024M -vga std -serial stdio
# VirtIO GPU/Net/Sound ile:
qemu-system-x86_64 -cdrom build/arctian.iso -boot d -m 2G -device virtio-vga \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 -device intel-hda -device hda-duplex -serial stdio
```

Kurulum akışını uçtan uca test etmek için (hedef disk en az **5 GiB** olmalıdır):

```sh
bash tools/test_install.sh      # ISO + sanal hedef disk ile QEMU başlatır
bash tools/run_installed.sh     # kurulu diski doğrudan açar
```

## Fiziksel diske yazma

```sh
python3 tools/arctian_writer.py --list
python3 tools/arctian_writer.py --disk /dev/sdb --yes
```

**Dikkat:** diske yazılacak dosya `build/arctian.img` olmalıdır. ISO'nun LBA 0'ında `0x55AA`
disk MBR imzası farklıdır; ham USB/disk olarak `arctian.img` kullanılmalıdır.

## `.wlf` SDK derleme

```sh
cd sdk/examples/merhaba && ../../wf build merhaba     # .wlf üretir
bash tools/build_wlf_examples.sh                      # tüm örnekler -> build/wlf/
```

`wf`, `gcc` + ELF bağlayıcısı kullandığı için **Windows'ta yerel olarak derlenmez**; Linux veya
WSL tarafında çalıştır (`wf.cmd` WSL'e köprüler). Ayrıntı: [sdk/README.md](../sdk/README.md).

## Sık karşılaşılan tuzaklar

- **`make` hiçbir şey yapmadan başarılı dönüyorsa:** Makefile başındaki `.DEFAULT_GOAL := all`
  satırı ve `-include $(BUILD)/...` kapsamı sayesinde `.d` dosyaları varsayılan hedefi ezmez.
- **`build.bat` karakter yutuyorsa:** betik CRLF satır sonuna sahip olmalıdır.
- **Bayrak değişti ama kod derlenmedi gibi görünüyor:** nesne dizini `build/obj-<hash>` olarak
  bayrağa göre ayrılmıştır; farklı bayraklar birbirini ezmez.
- **ISO yok ama `xorriso` hatası var:** `apt install xorriso`.
- **İmza anahtarı değişti:** `python3 tools/sig_keygen.py` sonrasında OS imajını yeniden derleyin ve
  varsa eski `.wlf` paketlerini `python3 tools/pkg_sign.py` ile yeniden imzalayın.
