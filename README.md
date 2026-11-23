# Arctian OS

x86_64 için sıfırdan yazılmış, modüler bir masaüstü işletim sistemi. Kendi önyükleyicisi,
kendi çekirdeği, kendi dosya sistemi (AFS), kendi TCP/IP yığını ve LVGL tabanlı bir masaüstü
kabuğu vardır. Çekirdek ve sistem modülleri harici bir mikroişletimci veya çekirdek içi
libc kullanmaz; Ring-3 kullanıcı alanı uygulamaları için ise `libWolf` (`.wlf`) SDK'sına
ek olarak **musl 1.2.5 + libstdc++ (C++20)** çapraz araç zinciriyle standart ELF64
POSIX ikilileri desteklenir.

**Sürüm: 0.9.2** — tek kaynak [`version.json`](version.json).

![Arctian masaüstü](docs/img/masaustu.png)

---

## Neler çalışıyor

| Alan | Durum |
|---|---|
| Önyükleme | MBR (USB/CD) + El Torito ISO (VESA/Bochs VBE, 64-bit uzun mod, ilerlemeye bağlı açılış animasyonu) ve **UEFI (`boot/uefi/`)** (OVMF/QEMU'da GOP + ACPI RSDP + ESP'den `KERNEL.ELF` + `ExitBootServices` → 64-bit çekirdek doğrulandı) |
| Modüller | `boot` → `main_kernel` → `install` → `kernel_plus` → `desktop`; her biri sabit adresli bağımsız `.bin` |
| Çok çekirdek (SMP) | MADT/LAPIC/AP trampoline, per-CPU GDT/TSS/IDT, `on_cpu` + `mutex`/`sem`/`pipe`/`mq` spinlock'ları ve IRQ-güvenli seri konsol (`-smp 4` sync/IPC/sched öz-testleri `EXC=0`) |
| Masaüstü | Dock, başlat menüsü, gerçek arama, pencereler (taşıma/yakınlaştırma/boyut hatırlama), sanal masaüstleri, hızlı ayarlar + OSD, bildirim merkezi, gece modu, çoklu hesap, iki ekran kilit/giriş arayüzü |
| Uygulamalar | Terminal, Dosya Yöneticisi, Metin, Foto, Takvim, Tarayıcı (Yerleşik olarak Arctian Browser), DuckStore, Ayarlar, Medya Oynatıcı, Görev Yöneticisi, Sistem Araçları, Çöp Kovası, Asistan (gömülü); Ofis paketi (Şafak, Mürekkep, Abaküs, Sahne, Mercek), `.wlf` paketleri ve Ring-3 ELF64 POSIX/C++20 ikilileri |
| Dosya sistemi | AFS v6 (`C:/`, LBA 65536+, 64-bit kapasite, günlükleme + fsck; v5 disklerle uyumlu), FAT32, ISO9660, USB MSD; AES-XTS disk şifreleme |
| Ağ | ARP/IPv4/TCP/UDP + DHCP, DNS, HTTP/HTTPS (mbedTLS), NTP, uygulama seviyesi güvenlik duvarı; 256 KB dairesel TCP alıcı halkası, RFC 7323 pencere ölçekleme ve e1000 toplu RDT/ITR ile 4 MB HTTP indirme **1,11 sn** (3,77 MB/sn, 68× hızlanma) |
| Sürücüler | e1000, PCnet, RTL8139, RTL8169, Atheros ath9k, Intel AX201 (gen3 ürün yazılımı yükleme; tarama/bağlantı iskelet), USB UHCI/EHCI/XHCI + HID + MSD, AHCI/ATA/ATAPI/NVMe, AC'97 ses, PS/2 klavye-fare, CMOS RTC (IRQ-güvenli), ACPI (Güç Kapat / uyku durumları), VESA |
| Uygulama modeli | Gömülü kabuk uygulamaları, `.wlf` paketleri (roller: `ui`, `cli`, `runtime`; dosya uzantısı eşlemesi "birlikte aç") ve **Ring-3 POSIX ELF64 (static-PIE)** ikilileri (`elfrun`) |
| Yalıtım (`.wlf`) | **B3 tamam:** `.wlf` uygulamaları **ring 3'te, kendi sayfa tablosunda (CR3)** çalışır (`W^X`, çekirdek sayfaları `U=0`); `wolf_api_t` trampoline tablosu + `SYS_WOLF_CALL`/`SYS_WOLF_WAIT` + LVGL broker kuyruğu ile GUI çizimi de izole alanda yürür. Kabuk ve gömülü uygulamalar tasarım gereği çekirdek yetkisindedir |
| POSIX & C++20 | **Ring-3 Linux x86-64 `SYSCALL/SYSRET` ABI:** ELF64 static-PIE yükleyici, `fork`/`execve`/`wait4`/`exit_group`, `mmap`/`munmap`/`mprotect` (JIT `PROT_EXEC`), `brk`, `clone`/TLS (`arch_prctl`)/`futex`/`pthread`/`std::thread`, `poll`/`epoll`, `AF_UNIX` (`socketpair`+`SCM_RIGHTS`)/`AF_INET` soketleri ve paylaşımlı ARGB8888 pencere/girdi köprüsü (`23/23 PASS`, `EXC=0`) |
| Güvenlik | Yetenek maskesi + dosya kapısı + komut kapısı + kullanıcı onay penceresi + karar deposu; RSA-2048/SHA-256 paket ve manifesto imzası |
| Güncelleme | İmzalı sürüm manifestosu, kapanışta/açılışta kurulum, A/B sistem yuvası ve geri dönüş |
| Mağaza | DuckStore: Python feed + web market sunucusu, OS istemcisi, `wf` komut satırı aracı |

Ekran görüntüleri QEMU'dan alınmıştır (1280×720, varsayılan açılış çözünürlüğü).

![Dosya yöneticisi](docs/img/dosya-yoneticisi.png)

![Terminal ile .wlf çalıştırma](docs/img/terminal-wlf.png)

## Hızlı başlangıç

Windows'ta tek adım (`tools/build-release.sh` aracılığıyla WSL Debian yerel ext4 kopyasında derler):

```bat
build.bat
```

Derleme bittiğinde:

```
build\arctian.iso    <- USB / CD / kalıp için (CD001, El Torito önyüklenebilir)
build\arctian.img    <- ham disk imaji
```

ISO'yu QEMU'da denemek için:

```sh
make qemu-iso
# veya
qemu-system-x86_64 -cdrom build/arctian.iso -boot d -m 512M -vga std
```

Kurulumdan başka bir şey yapmana gerek yok: ISO önyüklendiğinde önyükleme menüsü
("Kur / Dene") gelir. Ayrıntılar: [docs/derleme.md](docs/derleme.md)

## Kendi uygulamanı yazmak

**1) `.wlf` uygulaması (`libWolf`):**

```sh
cd sdk/examples/merhaba
../../wf build merhaba
```

Ofis uygulamaları (`sdk/office/`) ve üçüncü parti paketler bu yolla derlenir; sistemde
`wf install <paket>` ile kurulur. SDK ve `wolf_api_t` için: [sdk/README.md](sdk/README.md).

**2) C / C++20 POSIX ELF64 uygulaması (`sdk/toolchain/`):**

```sh
bash sdk/toolchain/build-toolchain.sh     # pure-musl 1.2.5 + libstdc++ 14.2 sysroot
sdk/toolchain/arctian-cxx -O2 app.cpp -o app.elf
# Arctian terminalinde:
elfrun C:/yol/app.elf
```

Pencereli Ring-3 uygulamalar için `sdk/toolchain/include/arctian/gfx.h` paylaşımlı
ARGB8888 tuval + olay köprüsü sağlar. Gömülü C API referansı: [docs/uygulama-gelistirme.md](docs/uygulama-gelistirme.md).

## Depo yapısı

```
boot/          stage1 (MBR), stage1_iso (El Torito), stage2 (NASM) ve uefi/ (PE/COFF EFI)
main_kernel/   GDT/IDT, sayfalama, VMM, modül yükleyici, syscall, ring3 geçişi
install/       kurulum sihirbazı (disk bölümleme + AFS biçimlendirme + kalıp)
kernel_plus/   sürücüler, ağ yığını, SMP zamanlayıcı/iplik, yetenek kapıları, TLS, posix/ alt sistemi
desktop/       masaüstü kabuğu (LVGL), gömülü uygulamalar, .wlf yükleyici/VM, posix_win köprüsü, tema
lib/           AFS v6, FAT32, ISO9660, blok katmanı, ses/medya, PNG/SVG, malloc, serial/klog
include/       kamu başlıkları (asi.h, app.h, wlf.h, posix_gfx.h, memory_map.h, ...)
sdk/           libWolf SDK, wf CLI, örnekler ve toolchain/ (musl + C++20 çapraz araç zinciri)
store/         DuckStore sunucusu (feed + web market) ve dağıtım dosyaları
tools/         paketleme, imzalama, anahtar üretimi, font/duvar kağıdı üretimi, QEMU yardımcıları
tests/         UEFI, SMP, izolasyon ve POSIX/C++20 (tests/posix/test_suite.cpp) doğrulama paketleri
design/        arayüz şartnamesi, duvar kağıdı, logo, fontlar ve açılış animasyonu görselleri
third_party/   LVGL, mbedTLS (vendored — bkz. third_party/README.md)
docs/          mimari, derleme, uygulama geliştirme, durum defteri ve yol haritası
```

## Belgeler

- [docs/mimari.md](docs/mimari.md) — modüller, bellek haritası, önyükleme akışı, AFS v6, `.wlf` ve Ring-3 POSIX alt sistemi
- [docs/derleme.md](docs/derleme.md) — gereken araçlar, `make` hedefleri, `sdk/toolchain/` kurulumu, ISO ve QEMU doğrulaması
- [docs/uygulama-gelistirme.md](docs/uygulama-gelistirme.md) — gömülü ASI/LVGL API'si, `.wlf` geliştirme ve Ring-3 C++20/POSIX arayüzü
- [CHANGELOG.md](CHANGELOG.md) — sürüm kayıtları
- [docs/1.0-durum.md](docs/1.0-durum.md) — güncel hat bazında durum, ölçümler ve sınırlar
- [docs/1.0-yol-haritasi.md](docs/1.0-yol-haritasi.md) — 1.0 hazırlık kontrol listesi ve kanıtlar
- [docs/0.9-durum.md](docs/0.9-durum.md) — 0.9 geliştirme dalgası arşiv durumu

## Bilinen sınırlar

- **`.wlf` ve POSIX ELF64 uygulamaları donanımsal olarak yalıtılmıştır (ring 3 + ayrı CR3).**
  Çekirdek sayfaları kullanıcı alanında `U=0`, kod `X` (yazılamaz), veri/yığın `NX`'tir (`mprotect`
  ile `PROT_EXEC` istisnası JIT amaçlı desteklenir). `.wlf` tarafında tek aktif izole GUI
  oturumu ve poll tabanlı (`~5/2 ms`) broker kuyruğu kullanılır. Kabuk ve gömülü uygulamalar
  tasarım gereği çekirdekle aynı adres alanındadır. QuickJS ve Lexbor depoda yoktur;
  `desktop/apps/wolf_engine/main.c` yalnızca geriye dönük ABI yer tutucusudur.
- **Ağ sürücüleri kısmi:** e1000, PCnet, RTL8139/RTL8169 adaptörleri desteklenir; virtio-net yoktur.
  Kablosuz tarafta ath9k (AR928x) uçtan uca çalışır, Intel AX201 ürün yazılımını gerçek gen3
  akışıyla yükler ancak tarama ve bağlantı adımları iskelettir ve gerçek donanımda doğrulanmamıştır.
- **Canlı (RAM) masaüstü yok.** ISO'dan açılış kalıcı kurulum akışına gider; tam masaüstü
  oturumu diske kurulduktan sonra açılır.
- Windows üzerinde yerel `.wlf` / ELF64 derlemesi yoktur; Linux veya WSL üzerinden derlenir.
- İmza anahtarı `tools/sig_keygen.py` (`secrets.SystemRandom`) ile üretilir; geliştirme/test
  ortamı için `tools/keys/dev_rsa2048.txt` yerel tutulur (`.gitignore`). Yayın öncesi özel anahtarın
  (`include/arctian/sig_priv.h`) yayın ortamından ayrılması önerilir.
- Kod tabanında geçmiş hata taraması: [docs/bilinen-hatalar.md](docs/bilinen-hatalar.md).

## Lisans

CC BY 4.0 (Atıf 4.0 Uluslararası) — tam metin: [LICENSE](LICENSE). Ticari kullanım dahil
her türlü kullanımda (dağıtım, kopya, değiştirilmiş sürüm) eser sahibinin bildirilmesi
zorunludur: "Arctian OS — Copyright 2026 Wither Studio/B. Kerem Örki".
`third_party/` altındaki kütüphaneler (LVGL MIT; mbedTLS Apache-2.0)
kendi lisanslarıyla gelir; bkz. [third_party/README.md](third_party/README.md).
