/* Lock-free SPSC queues — AArch64
 * ldar/stlr only allow [Xn] or [Xn,#0] — compute address in a temp.
 * RawPacket=792, ConflatedUpdate=352
 */
    .text
    .align 2
    .global _spsc_try_push
    .global _spsc_try_pop
    .global _spsc_update_try_push
    .global _spsc_update_try_pop

.set RAW_SZ, 792
.set UPD_SZ, 352
.set OFF_HEAD, 0
.set OFF_TAIL, 64
.set OFF_CAP, 128
.set OFF_MASK, 136
.set OFF_SLOTS, 144

_spsc_try_push:
    add     x9, x0, #OFF_TAIL
    ldar    x2, [x9]                   /* tail */
    add     x10, x0, #OFF_HEAD
    ldar    x3, [x10]                  /* head */
    ldr     x4, [x0, #OFF_MASK]
    add     x5, x2, #1
    and     x5, x5, x4
    cmp     x5, x3
    b.eq    1f
    ldr     x6, [x0, #OFF_SLOTS]
    mov     x7, #RAW_SZ
    mul     x7, x2, x7
    add     x6, x6, x7
    mov     x8, #RAW_SZ
    mov     x11, x1
2:
    ldr     x12, [x11], #8
    str     x12, [x6], #8
    subs    x8, x8, #8
    b.ne    2b
    add     x2, x2, #1
    and     x2, x2, x4
    stlr    x2, [x9]
    mov     x0, #1
    ret
1:
    mov     x0, #0
    ret

_spsc_try_pop:
    add     x9, x0, #OFF_HEAD
    ldar    x2, [x9]
    add     x10, x0, #OFF_TAIL
    ldar    x3, [x10]
    cmp     x2, x3
    b.eq    1f
    ldr     x4, [x0, #OFF_MASK]
    ldr     x6, [x0, #OFF_SLOTS]
    mov     x7, #RAW_SZ
    mul     x7, x2, x7
    add     x6, x6, x7
    mov     x8, #RAW_SZ
    mov     x11, x1
2:
    ldr     x12, [x6], #8
    str     x12, [x11], #8
    subs    x8, x8, #8
    b.ne    2b
    add     x2, x2, #1
    and     x2, x2, x4
    stlr    x2, [x9]
    mov     x0, #1
    ret
1:
    mov     x0, #0
    ret

_spsc_update_try_push:
    add     x9, x0, #OFF_TAIL
    ldar    x2, [x9]
    add     x10, x0, #OFF_HEAD
    ldar    x3, [x10]
    ldr     x4, [x0, #OFF_MASK]
    add     x5, x2, #1
    and     x5, x5, x4
    cmp     x5, x3
    b.eq    1f
    ldr     x6, [x0, #OFF_SLOTS]
    mov     x7, #UPD_SZ
    mul     x7, x2, x7
    add     x6, x6, x7
    mov     x8, #UPD_SZ
    mov     x11, x1
2:
    ldr     x12, [x11], #8
    str     x12, [x6], #8
    subs    x8, x8, #8
    b.ne    2b
    add     x2, x2, #1
    and     x2, x2, x4
    stlr    x2, [x9]
    mov     x0, #1
    ret
1:
    mov     x0, #0
    ret

_spsc_update_try_pop:
    add     x9, x0, #OFF_HEAD
    ldar    x2, [x9]
    add     x10, x0, #OFF_TAIL
    ldar    x3, [x10]
    cmp     x2, x3
    b.eq    1f
    ldr     x4, [x0, #OFF_MASK]
    ldr     x6, [x0, #OFF_SLOTS]
    mov     x7, #UPD_SZ
    mul     x7, x2, x7
    add     x6, x6, x7
    mov     x8, #UPD_SZ
    mov     x11, x1
2:
    ldr     x12, [x6], #8
    str     x12, [x11], #8
    subs    x8, x8, #8
    b.ne    2b
    add     x2, x2, #1
    and     x2, x2, x4
    stlr    x2, [x9]
    mov     x0, #1
    ret
1:
    mov     x0, #0
    ret
