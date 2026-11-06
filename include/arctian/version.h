#ifndef ARCTIAN_VERSION_H
#define ARCTIAN_VERSION_H

/* Tek kaynak: version.json (Makefile derleme aninda okur). */
#ifndef ARCTIAN_VERSION_STR
#define ARCTIAN_VERSION_STR "0.0"
#endif
#define ARCTIAN_VERSION ARCTIAN_VERSION_STR

/* Varsayilan guncelleme kaynagi: GitHub surumlarindaki varlik koku.
 * Sozlesme: en son surumde "update.txt" adli bir varlik durur (imzali
 * manifest); moduller yine ayni surumde varlik olarak bulunur.
 * Taban "/download" ile bitiyorsa varlik kipi kullanilir (manifest
 * {taban}/update.txt), degilse eski DuckStore beslemesi ({taban}/v1/update).
 * Test/ayna icin derleme zamaninda baska URL verilebilir:
 *   -DARCTIAN_UPDATE_URL='"http://10.0.2.2:8091/download"'
 * Kullanicinin kalici secimi C:/etc/update.cfg icindeki url= satiridir. */
#ifndef ARCTIAN_UPDATE_URL
#define ARCTIAN_UPDATE_URL \
    "https://github.com/withermen/Arctian-OS/releases/latest/download"
#endif

#endif /* ARCTIAN_VERSION_H */
