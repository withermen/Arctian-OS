#ifndef ARCTIAN_DISKCRYPT_HDR_H
#define ARCTIAN_DISKCRYPT_HDR_H

#define DISKCRYPT_HDR_LBA   19968u
#define DISKCRYPT_MAGIC     0x52434421u
#define DISKCRYPT_VERSION   2u
/* 2. surum: state/progress alanlari eklendi. Kurulum yariada kesilirse
 * durum baslikta durur; dis "sifresiz gorunup icinde sifreli veri"
 * birakmaz. 1. surum basliklar okunmaya devam eder. */
#define DISKCRYPT_ST_FINAL    0u   /* sifreleme tamaminlanmis, kilitli/actik */
#define DISKCRYPT_ST_ENC      1u   /* kurulum suruyor: [base, progress) sifreli */
#define DISKCRYPT_ST_DEC      2u   /* kaldirma suruyor: [base, progress) duz */

#endif
