/* x86-64 SYSCALL / SYSRET (iretq donuslu) giris ve Ring-3 gecis koprusu.
 *
 * GS degismez kurali:
 *   GS_BASE = 0, KERNEL_GS_BASE = percpu_self() HER ZAMAN gecerlidir
 *   (hem kernel hem user modunda). SYSCALL girisinde yalnizca percpu
 *   syscall_kstack / syscall_usp alanlarini okumak icin swapgs yapilir ve
 *   4 komut sonra (IF=0 iken) derhal geri swapgs yapilir. Boylece IRQ,
 *   baglam degisimi, fork/clone/execve sirasinda GS hic kaymaz.
 */
.text

.globl arctian_syscall_entry
.type arctian_syscall_entry, @function
arctian_syscall_entry:
    /* IF=0 (FMASK tarafindan kapatildi), RCX=user RIP, R11=user RFLAGS */
    swapgs
    movq %rsp, %gs:8          /* percpu->syscall_usp = user RSP */
    movq %gs:0, %rsp          /* kernel stack top */
    pushq $0x1B               /* ss = GDT_USER_DATA | 3 */
    pushq %gs:8               /* rsp = user RSP */
    swapgs                    /* GS_BASE=0, KERNEL_GS_BASE=percpu geri yuklendi */
    pushq %r11                /* rflags */
    pushq $0x23               /* cs = GDT_USER_CODE | 3 */
    pushq %rcx                /* rip */

    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es

    movq %rsp, %rbx           /* callee-saved: posix_regs_t *regs */
    subq $512, %rsp
    andq $-16, %rsp
    fxsave (%rsp)

    movq %rbx, %rdi           /* posix_regs_t *regs */
    cld
    call posix_syscall_dispatch

    fxrstor (%rsp)
    movq %rbx, %rsp

.globl posix_iret_return
posix_iret_return:
    cli
    movw $0x1B, %ax
    movw %ax, %ds
    movw %ax, %es

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    iretq
.size arctian_syscall_entry, .-arctian_syscall_entry

/* void posix_enter_user(posix_regs_t *regs)
 * Verilen tam yazmac cercevesini kernel yiginina kopyalayıp Ring-3'e girer.
 * Ilk ELF baslatma, fork cocugu ve clone thread'i icin ortak yol. */
.globl posix_enter_user
.type posix_enter_user, @function
posix_enter_user:
    cli
    /* regs (160 bayt = 20 x 8) mevcut yigina kopyala ve iretq ile don */
    subq $160, %rsp
    movq %rdi, %rsi
    movq %rsp, %rdi
    movq $20, %rcx
    cld
    rep movsq
    jmp posix_iret_return
.size posix_enter_user, .-posix_enter_user

.section .note.GNU-stack,"",@progbits
