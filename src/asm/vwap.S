/* NEON top-10 VWAP: sum(p*q)/sum(q)
 * x0 = BookLevel*, x1 = count (≤10)
 * returns d0
 * BookLevel = {double price; double qty;} stride 16
 */
    .text
    .align 2
    .global _asm_vwap_top10

_asm_vwap_top10:
    movi    v0.2d, #0                  /* acc pq in d0 */
    movi    v1.2d, #0                  /* acc q  in d1 */
    cbz     x1, 9f
    cmp     x1, #10
    csel    x1, x1, xzr, ls
    mov     x2, #10
    cmp     x1, #10
    csel    x1, x1, x2, ls
    /* x1 = min(count, 10) already if count<=10; clamp: */
    cmp     x1, #10
    b.ls    1f
    mov     x1, #10
1:
    mov     x3, x0
2:
    ldr     d2, [x3]                   /* price */
    ldr     d3, [x3, #8]               /* qty */
    fmul    d4, d2, d3
    fadd    d0, d0, d4
    fadd    d1, d1, d3
    add     x3, x3, #16
    subs    x1, x1, #1
    b.ne    2b
    /* if sum_q == 0 return 0 */
    fcmp    d1, #0.0
    b.eq    9f
    fdiv    d0, d0, d1
    ret
9:
    fmov    d0, xzr
    ret
