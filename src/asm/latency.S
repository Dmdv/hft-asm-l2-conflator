/* Latency ring push — AArch64 */
    .text
    .align 2
    .global _latency_try_push

.set L_OFF_W, 0
.set L_OFF_DROP, 64
.set L_OFF_CAP, 128
.set L_OFF_SAMP, 144

_latency_try_push:
    add     x9, x0, #L_OFF_W
    ldar    x2, [x9]
    ldr     x3, [x0, #L_OFF_CAP]
    cmp     x2, x3
    b.hs    2f
    ldr     x4, [x0, #L_OFF_SAMP]
    str     x1, [x4, x2, lsl #3]
    add     x5, x2, #1
    stlr    x5, [x9]
    mov     x0, #1
    ret
2:
    add     x9, x0, #L_OFF_DROP
    ldar    x2, [x9]
    add     x2, x2, #1
    stlr    x2, [x9]
    mov     x0, #0
    ret
