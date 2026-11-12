; Arctian UEFI onyukleyici giris noktasi (nasm -f win64).
;
; Firmware bizi MS ABI ile cagirir: RCX = ImageHandle, RDX = SystemTable.
; Kendi yiginimiza gecip efi_main'i cagiryoruz; donerse CPU'yu duraklatiyoruz
; (paket 1'de kernel yok, BootServices cagrilmayacak).

section .data align=64
efi_stack: times 65536 db 0   ; 64 KB kendi yiginimiz (win64 COFF'da .bss yok)

section .text
global efi_entry
extern efi_main

efi_entry:
    lea  rsp, [rel efi_stack + 65536]
    and  rsp, -16              ; 16 bayt hizala
    sub  rsp, 0x20             ; 32 bayt MS ABI golge alani
    call efi_main              ; RCX/RDX zaten yerinde
.halt:
    cli
    hlt
    jmp .halt                  ; donus olmamali; olsin diye bekler
