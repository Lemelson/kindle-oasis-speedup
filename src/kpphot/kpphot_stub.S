@ kpphot asm stubs (ARM state). Entered by `ldr pc, [pc, #-4]` written over libcairo 0x5debc/0x5dec0.
@ State there (libcairo 1.10 _get_image_surface, FW 5.18.2): r3 = image stride (bytes), r4 = XImage*, r5 = image surface,
@ r6 = 0 (status), fp = frame; sp 8-byte aligned (the function makes AAPCS calls from here). lr, r0-r3, ip are dead
@ (overwritten before any read on every path from 0x5dec4).
    .syntax unified
    .arm
    .text
    .global hot_cairo_stub
    .hidden hot_cairo_stub
    .type hot_cairo_stub, %function
hot_cairo_stub:
    bic     r3, r3, #3              @ replayed: 0x5debc  bic r3, r3, #3
    str     r3, [fp, #-152]         @ replayed: 0x5dec0  str r3, [fp, #-152]
    mov     r0, fp
    mov     r1, r4
    bl      hot_cairo_get           @ AAPCS: r4-r11, sp preserved
    cmp     r0, #0
    ldr     ip, 1f
2:  add     ip, pc, ip              @ ip = hot_cairo_ret
    ldreq   pc, [ip]                @ fallback: original loop at 0x5dec4 (mov r3, #0; ...)
    ldr     pc, [ip, #4]            @ done: original loop exit 0x5de2c
1:  .word   hot_cairo_ret - (2b + 8)
    .size hot_cairo_stub, . - hot_cairo_stub

@ libpixman 0x415bc __aeabi_ldivmod (static libgcc, ARM): {r0:r1 / r2:r3} -> quot r0:r1, rem r2:r3.
@ Entered by `ldr pc,[pc,#-4]` over 0x415bc/0x415c0 (only `bl 0x415bc` reaches them, 18 sites). Fast path only when
@ divisor and dividend are sign-extended 32-bit values, divisor != 0 and not (INT_MIN / -1): SDIV + MLS give the same
@ truncated quotient and remainder. Otherwise r0-r3/lr untouched (only ip + flags used) and we continue in the original:
@ divisor 0 -> 0x415c8 (its zero handling), else -> 0x415e4 (generic path); neither depends on the flags we changed.
    .text
    .global hot_ldivmod_stub
    .hidden hot_ldivmod_stub
    .type hot_ldivmod_stub, %function
    .arch_extension idiv
hot_ldivmod_stub:
    orrs    ip, r2, r3
    beq     8f
    cmp     r3, r2, asr #31
    bne     9f
    cmp     r1, r0, asr #31
    bne     9f
    cmn     r2, #1
    bne     1f
    cmp     r0, #0x80000000
    beq     9f
1:  sdiv    ip, r0, r2
    mls     r2, ip, r2, r0
    mov     r0, ip
    asr     r1, r0, #31
    asr     r3, r2, #31
    bx      lr
8:  ldr     ip, 6f
7:  add     ip, pc, ip
    ldr     pc, [ip]
9:  ldr     ip, 4f
5:  add     ip, pc, ip
    ldr     pc, [ip, #4]
6:  .word   hot_ldiv_ret - (7b + 8)
4:  .word   hot_ldiv_ret - (5b + 8)
    .size hot_ldivmod_stub, . - hot_ldivmod_stub
    .section .note.GNU-stack,"",%progbits
