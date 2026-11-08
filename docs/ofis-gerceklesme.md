# Ofis Uygulamalarını Gerçekleştirme Planı

Beş ofis uygulaması (Şafak, Mürekkep, Abaküs, Sahne, Mercek) 0.8'de arayüz
olarak yayınlandı ama hepsi vitrin: içerikler kaynağa gömülü, düğmeler
"v0.8'de yok" diyor. Bu dosya, her birini gerçek çalışır uygulamaya
dönüştürmek için yapılan etüdün sonucu. Sıra: önce altyapı açıkları (bir
kerelik, host tarafı), sonra uygulamalar kolaydan zora.

## Altyapı açıkları (host / libWolf / ABI)

Uygulamalar native x86-64 çalışıyor; hız ve bellek sorunu yok. Eksik olan
şey API'de yok ya da yarım:

Not — sözleşme tuzakları (Şafak yazarken yakalandı, ikisi tekrar etmesin
diye buraya): `fs->write` **0 = başarı** döndürür (bayt sayısı değil;
`afs_write_handle` ve terminal `== 0` kullanıyor); `time->now` **1 = başarı**
(bool, `clock_now`'dan); `fs_list`/`list_dir` **0 = başarı**, tarama döngüsü
`!= 0` ile kırılmalı (`<= 0` ilk başarılı satırda döngüyü bitirir — Şafak'ın
taraması bir süre bu yüzden hiç dosya bulamadı). `wolf.h` yorumları güncellendi.

0. **Yeniden çizim slot sızıntısı (Abaküs kesilmesinin kökü) — düzeltildi,
   QEMU'da doğrulandı.** Host'un
   widget slot tablosu `wolf_clear`/`wolf_destroy`'ta LVGL nesneleri
   silinirken boşaltılmıyordu: kabuk açılışta relayout tetikleyince
   (tam ekran pencere boyutlandırması) Abaküs `wolf_clear + build` ile
   her şeyi yeniden üretiyor, yeni nesneler boş slot bulamayıp sessizce
   düşüyordu — çizim ~7. satırda kesiliyordu. Emülatörde relayout no-op
   olduğu için görüntü tamdı; hata yalnız gerçek OS'ta görünüyordu.
   Çözüm: silmeden önce alt ağaç slotlarını serbest bırakan
   `slot_free_tree` (h_clear/h_destroy), limit 256→512. Eski slotların
   ölü nesne tutması da (id çözümlemede kullanılırsa çökme riski)
   böylece kapandı.

0b. **Onay diyalogu ile 5 saniyelik açılış bekçisi çarpışıyordu — düzeltildi.**
   Kabuk, uygulamanın `entry()` çağrısını `recover_begin(ctx, 5000)` ile
   sarar; ilk dosya erişimi yol konsenti istersen `consent_ask` aynı çağrının
   içinde `consent_pump` ile kullanıcı kararını bekler. Bekleme 5 saniyeyi
   aşınca bekçi "Uygulama yanit vermedi" deyip sağlam uygulamayı öldürüyordu
   (Şafak'ın gerçek OS'taki ilk açılışında görülen buydu; emülatörde konsent
   penceresi çizilmediği için ortaya çıkmadı). Çözüm: `recover_touch(ms)` —
   bekleme döngüsü her turunda açılış son tarihini bir tur daha uzatır.
   Bekçinin asıl amacı (takılı kalan uygulamayı yakalamak) korunur; onay
   penceresinin kendi 48 saniyelik güvence sınırı (CONSENT_ITERS) değişmedi.

1. **Klavye girişi bağlı değil.** Masaüstü pencere başına odak grubunu
   (`fgroup`) uygulamanın `entry()` içinde bağladığı gruptan alıyor
   (launch.c), ama wlf köprüsü hiç grup oluşturmuyor. `wolf_input`
   üretiliyor, tıklandığında LVGL onu işaretçi girdisinin grubuna veriyor;
   tuşlar ise klavye girdisinin grubuna gidiyor. İkisi farklı olduğu için
   yazı yazılamıyor. Çözüm: wlf_host oturum başına bir `lv_group` açsın,
   `h_input`'ta nesneyi eklesin ve pencerenin `fgroup`'unu bağlasın.
   Ek kural: tam ekran ofis penceresi kapanınca klavye grubu geri
   dönmeli (main.c'deki pencere-üstte mantığı bunu zaten yapıyor).

2. **Çok satırlı girdi yok.** `h_input` hep `one_line=true` ve 200 karakter
   sabit. Stil API'sine iki özellik eklenir (ABI kırılmaz, prop sona
   eklenir): `WOLF_SP_INPUT_ONE_LINE` (0 = çok satırlı) ve
   `WOLF_SP_INPUT_MAXLEN`. Mürekkep'in düz metin sayfaları için şart.

3. **Dosyayla açma zinciri UI rollerinde kopuk.** Dosya yöneticisi
   çift tıkla `arctian_shell_open_file()` çağırıyor; uzantıyı `handles`
   ile ilan eden kurulu .wlf bulunca uygulamayı açıyor ve yolu
   `arctian_open_target`a yazıyor. Ama hedef rol=ui ise yol hiçbir yere
   ulaşmıyor (yalnız cli/runtime terminal üzerinden `wf run ad dosya`
   ile alıyor). Çözüm: ui dalında da hedef yol açılış argümanı olarak
   `wlf_host_set_arg`a verilsin; uygulama `wolf_arg()` ile okur. Böylece
   .txt çift tıklandığında Abaküs'ün .xlsx ilan etmesi gibi davranışlar
   çalışır.

4. **Uygulamadan uygulamaya dosya taşıma yok.** `wolf_open_app(ad)` yol
   taşımıyor; `wolf_exec` beyaz listesi yalnız bilgi komutları. ABI'ye
   sona eklenecek: `open_app_file(ad, yol)` (şafak'ın "son kullanılanlar"
   satırları ve kartlardaki "PDF aç" bunu kullanacak).

5. **Dosya boyutu yok.** `fs_list` yalnız ad + dizin bayrağı veriyor;
   okuma tamponunu boyutlandırmak için `fs_size(yol)` (asi_stat_t zaten
   size taşıyor, köprüsü kolay).modification time ise AFS'de hiç
   yok; "son kullanılanlar" bunun yerine Şafak'ın kendi tuttuğu küçük
   bir liste dosyasıyla (`C:/Ofis/son.txt`) çözülür — açılan her dosya
   başa eklenir.

6. **Kaydetme onayı / dosya seçicisi.** Ayrı OS diyalogu yerine uygulama
   içi dosya tarayıcı yeterli (listbox + fs_list ile zaten yapılabilir);
   OS'a "farklı kaydet" sandığı eklemek şimdilik gerekmiyor.

Ayrıca not: `lib/png.c` içinde tam bir DEFLATE çözücü (stored + fixed +
dynamic) var; zip tabanlı ofis biçimlerinin yolu açık. Uygulamalar
freestanding olduğu için bu kodu link edemiyorlar — ortak bir
`office/common/` altına küçük bir inflate kopyası konur (ya da ileride
host'a `fs_inflate` API'si eklenir; ilk yolu tercih ediyorum, host
yüzeyi büyüsün istemem).

## Uygulama uygulama durum ve plan

### Şafak (başlangıç ekranı) — en kolay, altyapıyı da test eder
**Durum (0.9): tamamlandı ve QEMU'da doğrulandı.** Liste `C:/Belgeler/son.txt`
(düşüş `C:/son.txt`) dosyasından geliyor, satır `GG.AA SS:MM|yol`; tarama
Belgeler/Masaustu/Indirilenler klasörlerini tarıyor; satır tiki belgeyi
turune gore ilgili uygulamayla aciyor (`open_app_file`); arama kutusu gercek
girdi, klavye ile yaziliyor ve canli suzüyor. Konsent diyalogu + 5 sn bekci
cadismasi `recover_touch` ile cozuldu (madde 0b).
Bugun: kartlar ve "son kullanılanlar" sabit; W/X/P/D rayları `open_app`
ile gerçekten uygulama açıyor (tek çalışan şey bu).
Yapılacak:
- Son kullanılanlar, Şafak'ın kendi tuttuğu `C:/Ofis/son.txt` listesinden
  gelsin (her açılışta dosya başa eklenir; satır = yol). Liste
  doğrulanırken `fs_exists` ile ölü yollar atılır; geri kalan `fs_list`
  taramasıyla (Belgeler/Masaustu/Indirilenler) tamamlanır. Uzantıdan
  tür ikonu (W/X/P/PDF) belirlensin.
- Satıra çift tık → `open_app_file` ile ilgili ofis uygulamasına yol
  taşınsın; boş kartlar uygulamayı argümansız açsın.
- Arama kutusu klavye girişi alacak (madde 1), yazıldıkça ad araması
  yapsın.

### Abaküs (tablo) — formüller gerçek olsun
**Durum (0.9): tamamlandı; emülatör ve QEMU'da (gerçek OS) doğrulandı.** fx çubuğu artık gerçek
`wolf_input`; Enter (veya "Uygula") değeri hücreye yazar, seçim bir alta
düşer, izgara yeniden hesaplanır. Formül ayrıştırıcı: dört işlem, parantez,
hücre adresi, `TOPLA`/`ORT` (aralık ve liste); döngüsel başvuru `#CEVRE!`,
geçersiz ifade `#HATA!`. Sayısal giriş Türkçe biçimi de anlar ("8.500" →
8500, "1.234,56" → 1234.56); çıktı binlik noktalı ("102.000"). Aç/Kaydet
CSV'yi `wolf_arg()` yoluyla (Şafak listesinden gelen dosya) veya
`C:/belgeler/tablo.csv` üzerinden okur/yazar; başlık ve durum satiri
(secili sutunun sayim/toplam/ortalamasi) gercektir.
Bunun için ABI'ye bir alan eklendi: `on_enter` (girdide `LV_EVENT_READY`);
tablo sonuna eklenir, eski paketler `HAS()` ile görmezden gelir — iki
`wolf.h` + iki `libwolf` kopyası + `wlf_host.c` birlikte güncellendi.
**Gerçek OS (QEMU) çökmesi — kök neden bulundu ve düzeltildi:** Abaküs
açılışta `[isr] EXC int=0xD` ile devriliyordu. Teshis zinciri (rip her
koşuda `lv_obj_get_screen+0x13`, `rdi=0xFFFF0000FFFF` piksel deseni, yigin
izi `h_input → lv_textarea_create`) LVGL'nin yeni ayirdigi nesneye piksel
verisinin yazilmis oldugunu gosteriyordu. Kok neden uygulamada degil
kabukta: `windows.c` boyut degisikligini `LV_EVENT_SIZE_CHANGED` icinden
**senkron** olarak uygulamaya bildiriyordu; Abaküs'ün `on_resize`'i 200+
nesneyi silip yeniden kurarken LVGL yerlestirmesinin ortasina giriyor, ic
yapiyi boziyordu. Cozum: bildirim 120 ms'lik tek atimlik zamanlayiciyla
yerlestirim sonrasina tasindi (ust uste boyut degisimleri tek kurulumda
birlesir; dosya yoneticisinin ic kalibi artik kabuk duzeyinde). Abaküs
tarafi da boyut ayniysa yeniden kurmayi atliyor ve yazilmamis formulu
(`g_dirty`) kurulumdan once hucreye dokuyor. ISR'nin kayit+yigin dokumu
ilerideki sessiz cokmeler icin kalici birakildi.
Kalan: .xlsx salt-okur ayristirma ikinci faz (zip+inflate+XML, common
kütüphaneye bağlı); coklu sayfa (Sayfa 2) dekoratif.

### Sahne (sunum) — metin düzenleme + dosya
Bugün: küçük resme tıkla slayt değişimi çalışıyor; içerik sabit.
Yapılacak:
- Başlık/madde metinleri yer tutucu etiketlerden gerçek veriye geçsin;
  çift tık → satır içi `wolf_input`, yazılan slayt modeline işlensin,
  küçük resim başlığı tazelenensin.
- Belge dosyası: tek dosya, basit satır biçimi (`# baslik`, `- madde`);
  kaydet/aç `fs_write/fs_read` ile. Pptx ikinci faz.
- "Yeni slayt" ve silme ribondan çalışır hale gelsin.

### Mürekkep (belge) — düz metin editörü olarak başla
Bugün: her şey dekor; tek hareket sekme çizgisi.
Yapılacak:
- Sayfa alanı çok satırlı `wolf_input` olsun (madde 2): C:/Belgeler'deki
  .txt/.md dosyalarını açıp kaydedebilsin; kelime/satır sayacı durum
  çubuğundan gerçek olsun.
- Biçimlendirme çubuğu (B/I/U, başlık stilleri) markdown imleri
  metne girsin; kalıcı "zengin metin" modeli ikinci faz.
- Gerçek docx salt-okur (zip + document.xml'den düz metin) üçüncü faz.

### Mercek (PDF) — en zor, dürüst sınırla
Bugün: sayfa çevirme ve zoom tek örnek içerik üzerinde numara değiştiriyor.
Yapılacak (gerçekçi sıra):
- PNG görüntüleri gerçekten açsın (`wolf_image` diskten okuyup çözüyor;
  çok sayfalı PDF yerine önce gerçek görüntü).
- Basit PDF alt kümesi: PDF sözdizimi ayraçlarıyla sayfa sayısı +
  FlateDecode içerik akışından BT/Tj konumlandırmalı metin çıkarımı;
  sayfayı beyaz panel + konumlu etiketler olarak çiz. Yazı tişi
  sınırlı, ama kendi ürettiğimiz PDF'ler ve sade metin PDF'leri açılır.
- Karmaşık PDF'lerde "bu belge desteklenmiyor" uyarısı — sessiz boş
  sayfadan iyidir. Gerçek render motoru bu planın dışında.

## Sıra

1. Host: odak grubu + çok satırlı girdi + ui-argüman zinciri +
   `open_app_file`/`fs_size` (tek ABI sürümü, v3 zaten "sona
   ekle" sözü veriyor).
2. Şafak'ı gerçek dosyalara bağla (zincirin tamamını uçtan uca test eder).
3. Abaküs düzenleme + formüller; Sahne metin düzenleme + dosya.
4. Mürekkep düz metin editörü.
5. Mercek PNG + basit PDF.
6. Ortak ofis dosya biçimi ve simge/uzantı ilanları (`handles`), dosya
   yöneticisi çift tık entegrasyonu.

Her fazdan sonra wf shot + QEMU ekran görüntüsüyle doğrulama; host
değişiklikleri OS sürümünü gerektirdiği için 1. madde tek pakette
yayınlanmalı.
