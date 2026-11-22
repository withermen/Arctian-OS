# Arctian OS

x86_64 için sıfırdan yazılmış, modüler bir masaüstü işletim sistemi. Kendi önyükleyicisi,
kendi çekirdeği, kendi dosya sistemi (AFS), kendi TCP/IP yığını ve LVGL tabanlı bir masaüstü
kabuğu vardır. Üçüncü parti bir mikroişletimci veya libc kullanılmaz.

**Sürüm: 0.9.2** — tek kaynak [`version.json`](version.json).

![Arctian masaüstü](docs/img/masaustu.png)

---

## Neler çalışıyor

| Alan | Durum |
|---|---|
| Önyükleme | MBR (USB/CD) + El Torito ISO, VESA/Bochs VBE ile çerçeve tamponu, 64-bit uzun mod; çekirdek yüklenirken gerçek ilerlemeye bağlı açılış animasyonu |
| Modüller | `boot` → `main_kernel` → `install` → `kernel_plus` → `desktop`; her biri sabit adresli bağımsız `.bin` |
| Masaüstü | Dock, başlat menüsü, gerçek arama, pencereler (taşıma/yakınlaştırma/boyut hatırlama), sanal masaüstleri, hızlı ayarlar + OSD, bildirim merkezi, gece modu, çoklu hesap, kilit ekranı |
| Uygulamalar | Terminal, Dosya Yöneticisi, Tarayıcı (iki motor: Wolf Engine ve Lexbor), DuckStore, Ayarlar, Medya Oynatıcı, Görev Yöneticisi, Sistem Araçları, Çöp Kovası, Asistan (gömülü); Ofis paketi (Şafak, Mürekkep, Abaküs, Sahne, Mercek) ve `.wlf` olarak kurulan üçüncü parti uygulamalar |
| Dosya sistemi | AFS v4 (`C:/`), FAT32, ISO9660, USB MSD; fsck benzeri bütünlük denetimi, AES-XTS disk şifreleme |
| Ağ | ARP/IPv4/TCP/UDP + DHCP, DNS, HTTP/HTTPS (mbedTLS), NTP, uygulama seviyesi güvenlik duvarı |
| Sürücüler | e1000, PCnet, RTL8139, RTL8169, Atheros ath9k, Intel AX201 (gen3 ürün yazılımı yükleme; tarama/bağlantı iskelet), USB UHCI/EHCI/XHCI + HID + MSD, AHCI/ATA/ATAPI/NVMe, AC'97 ses, PS/2 klavye-fare, CMOS RTC, ACPI (Güç Kapat / uyku durumları), VESA |
| Uygulama modeli | **Her uygulama bir `.wlf`'tir** (gömülü çekirdek uygulamalar hariç); roller: `ui`, `cli`, `runtime`; dosya uzantısı eşlemesi ("birlikte aç") |
| Yalıtım | `.wlf` uygulamaları **ring 3'te, kendi sayfa tablosunda (CR3)** çalışır: çekirdek eşlemeleri kullanıcı alanında U=0, kod yazılamaz/X, veri-yığın NX (W^X); çıkış ve CPU istisnası çekirdeği çökertmeden uygulamayı kapatır. Kabuk ve gömülü sistem uygulamaları tasarım gereği çekirdek yetkisindedir |
| Güvenlik | Yetenek maskesi + dosya kapısı + komut kapısı + kullanıcı onay penceresi + karar deposu; RSA-2048/SHA-256 paket ve manifesto imzası |
| Güncelleme | İmzalı sürüm manifestosu, kapanışta/açılışta kurulum, A/B sistem yuvası ve geri dönüş |
| Mağaza | DuckStore: Python feed + web market sunucusu, OS istemcisi, `wf` komut satırı aracı |

Ekran görüntüleri QEMU'dan alınmıştır (1280×720, varsayılan açılış çözünürlüğü).

![Dosya yöneticisi](docs/img/dosya-yoneticisi.png)

![Terminal ile .wlf çalıştırma](docs/img/terminal-wlf.png)

## Hızlı başlangıç

Windows'ta tek adım (WSL + Debian üzerinden derler):

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

`.wlf` bir uygulamayı derlemek için `wf` yeterli:

```sh
cd sdk/examples/merhaba
../../wf build merhaba
```

Ofis uygulamaları (`sdk/office/`) bu yolla derlenip GitHub sürümüne konur; sistemde
`wf install abakus` gibi adıyla kurulur. Katalog `wf store` ile başka bir adrese
çevrilebilir; yeni uygulama eklemek için sistemi yeniden derlemeye gerek yok.

SDK, örnekler ve çalışma zamanı arayüzü (`wolf_api_t`) için:
[sdk/README.md](sdk/README.md) · C uygulaması API referansı:
[docs/uygulama-gelistirme.md](docs/uygulama-gelistirme.md)

## Depo yapısı

```
boot/          stage1 (MBR), stage1_iso (El Torito), stage2 — NASM
main_kernel/   GDT/IDT, sayfalama, modül yükleyici, syscall, ring3 iskeleti
install/       kurulum sihirbazı (disk bölümleme + AFS biçimlendirme + kalıp)
kernel_plus/   sürücüler, ağ yığını, zamanlayıcı/işlemci/iplik, yetenek kapıları, TLS
desktop/       masaüstü kabuğu (LVGL), uygulamalar, .wlf yükleyici/VM, tema
lib/           AFS, FAT32, ISO9660, blok katmanı, ses/medya, PNG/SVG, malloc, printf
include/       kamu başlıkları (asi.h, app.h, wlf.h, memory_map.h, ...)
sdk/           libWolf SDK, wf CLI, örnek uygulamalar
store/         DuckStore sunucusu (feed + web market) ve dağıtım dosyaları
tools/         paketleme, imzalama, font/duvar kağıdı üretimi, QEMU yardımcıları
design/        arayüz şartnamesi, duvar kağıdı, logo ve açılış animasyonu görselleri
third_party/   LVGL, mbedTLS (vendored — bkz. third_party/README.md)
docs/          mimari, derleme, uygulama geliştirme
```

## Belgeler

- [docs/mimari.md](docs/mimari.md) — modüller, bellek haritası, önyükleme akışı, AFS,
  `.wlf` modeli, yetenek ve imza modeli
- [docs/derleme.md](docs/derleme.md) — gereken araçlar, `make` hedefleri, ISO doğrulaması,
  QEMU ile çalıştırma, hata ayıklama varyantları
- [docs/uygulama-gelistirme.md](docs/uygulama-gelistirme.md) — C uygulama API'sinin tamamı
  (ASI, LVGL, fs/net/audio/güç/mağaza) ve tam örnekler
- [CHANGELOG.md](CHANGELOG.md) — sürüm kayıtları
- [docs/0.9-durum.md](docs/0.9-durum.md) — 0.9 dalgası hat bazında durum ve
  test kapsamı

## Bilinen sınırlar

- **`.wlf` uygulamaları donanımsal olarak yalıtılmıştır (ring 3 + ayrı sayfa
  tablosu).** Masaüstü bir `.wlf`'i çekirdeğe verir; çekirdek imajı doğrular,
  kendi PML4'ünü kurar (çekirdek sayfaları bu alanda U=0 → uygulama çekirdek
  belleğini okuyamaz/yazamaz), kodu X, veri ve yığını NX olarak eşler ve
  `iretq` ile CPL3'e düşer. Uygulama yalnız `int 0x80` syscall'larını görür;
  `SYS_EXIT`'i veya bir CPU istisnası yalnız kendi penceresini kapatır,
  sistem ayakta kalır. Kanıt: `bash tests/isolation/run.sh` (gömülü PIC
  imajla yükleme + ring 3 çıktısı + çıkış kanca + masaüstünün devamı).
  **Açık kalan:** libWolf GUI API'sinin kullanıcı tarafı köprüsü (`wolf_api_t`
  trampolinleri + olay modeli) henüz yok; bu yüzden GUI'li native `.wlf`
  uygulamaları yalıtılmış ortamda arayüzlerini çizemez (uygulama `wolf_ready()`
  = 0 görüp dürüst bir hata koduyla çıkar). Kabuk ve gömülü sistem
  uygulamaları tasarım gereği kabukla aynı adres alanındadır; script (VM)
  `.wlf` uygulamaları makine kodu yürütmeyen yorumlayıcıda çalışır.
- **Ağ sürücüleri kısmi:** e1000, PCnet, RTL8139/RTL8169 gibi eski PCI adaptörleri
  desteklenir; virtio-net yoktur. Kablosuz tarafta ath9k (AR928x) uçtan uca çalışıyor,
  Intel AX201 ürün yazılımını gerçek gen3 akışıyla yüklüyor ancak tarama ve bağlantı
  adımları iskelet ve gerçek donanımda doğrulanmadı. Wi-Fi kapsamı bu iki çipten
  ibarettir; modern kartların çoğu desteklenmez.
- **Canlı (RAM) masaüstü yok.** ISO'dan açılış kalıcı kurulum akışına gider; tam
  masaüstü oturumu diske kurulduktan sonra açılır. Kurulum yapmadan denemek için
  ayrı bir salt-okunur canlı oturum henüz yok. Gelecekte de eklenmeyecektir çünkü
  yapımcıya göre(Yani bana göre) bu gereksiz birşeydir. Birisi iso yu test ediyorsa 
  kendi bilgisayarına kurmak istiyor demektir.
- Windows üzerinde yerel `.wlf` derlemesi yok (ELF bağlayıcısı gerekiyor); Linux/WSL
  tarafında `wf` ile derlenir.Sebebi ise windows ortamı derleme yapmaya uygun bulunmamıştır.
- Depodaki imza anahtarı `tools/keys/dev_rsa2048.txt` bir **geliştirme** anahtarıdır;
  yayın öncesi `tools/sig_keygen.py` ile yenisi üretilmelidir.
- Kod tabanında bilinen hataların tam listesi: [docs/bilinen-hatalar.md](docs/bilinen-hatalar.md).
  Düzelttikçe oradan düşürüyorum; bir yere dokunmadan önce oraya göz atmakta fayda var.

## Lisans

CC BY 4.0 (Atıf 4.0 Uluslararası) — tam metin: [LICENSE](LICENSE). Ticari kullanım dahil
her türlü kullanımda (dağıtım, kopya, değiştirilmiş sürüm) eser sahibinin bildirilmesi
zorunludur: "Arctian OS — Copyright 2026 Wither Studio/B. Kerem Örki".
`third_party/` altındaki kütüphaneler (LVGL MIT; mbedTLS Apache-2.0)
kendi lisanslarıyla gelir; bkz. [third_party/README.md](third_party/README.md).
