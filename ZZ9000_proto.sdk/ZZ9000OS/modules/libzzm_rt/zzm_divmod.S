.syntax unified
.arm
.text
.align 2
.global __aeabi_ldivmod
.type __aeabi_ldivmod, %function
/* AAPCS: numerator r0:r1, denominator r2:r3; quotient r0:r1, remainder r2:r3. */
__aeabi_ldivmod:
    push    {r4-r11, lr}
    mov     r4, r0
    mov     r5, r1
    mov     r6, r2
    mov     r7, r3
    mov     lr, #0
    cmp     r5, #0
    bge     1f
    eor     lr, lr, #3
    rsbs    r4, r4, #0
    rsc     r5, r5, #0
1:
    cmp     r7, #0
    bge     2f
    eor     lr, lr, #1
    rsbs    r6, r6, #0
    rsc     r7, r7, #0
2:
    orrs    r0, r6, r7
    beq     6f
    mov     r8, #0
    mov     r9, #0
    mov     r10, #0
    mov     r11, #0
    mov     r12, #64
3:
    mov     r0, r5, lsr #31
    mov     r5, r5, lsl #1
    orr     r5, r5, r4, lsr #31
    mov     r4, r4, lsl #1
    mov     r9, r9, lsl #1
    orr     r9, r9, r8, lsr #31
    mov     r8, r8, lsl #1
    orr     r8, r8, r0
    mov     r11, r11, lsl #1
    orr     r11, r11, r10, lsr #31
    mov     r10, r10, lsl #1
    cmp     r9, r7
    blo     5f
    bhi     4f
    cmp     r8, r6
    blo     5f
4:
    subs    r8, r8, r6
    sbc     r9, r9, r7
    orr     r10, r10, #1
5:
    subs    r12, r12, #1
    bne     3b
    tst     lr, #1
    beq     7f
    rsbs    r10, r10, #0
    rsc     r11, r11, #0
7:
    tst     lr, #2
    beq     8f
    rsbs    r8, r8, #0
    rsc     r9, r9, #0
8:
    mov     r0, r10
    mov     r1, r11
    mov     r2, r8
    mov     r3, r9
    pop     {r4-r11, pc}
6:
    mov     r0, #0
    mov     r1, #0
    mov     r2, #0
    mov     r3, #0
    pop     {r4-r11, pc}
.size __aeabi_ldivmod, . - __aeabi_ldivmod
