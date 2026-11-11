/* Acilis animasyonu gorselleri (tools/bootanim_gen.py uretir).
 * 0xAARRGGBB, satir satir. Kaynak dosyalar design/bootanim altinda. */
.section .rodata
.global banim_logo_argb
.global banim_title_argb
.align 16
banim_logo_argb:
.incbin "design/bootanim/logo.argb"
.align 16
banim_title_argb:
.incbin "design/bootanim/title.argb"
