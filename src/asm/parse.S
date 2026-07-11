/* Parse is implemented in parse_fast.c (IEEE float + schema robustness).
 * This file remains so the asm/ unit is complete for the build graph.
 * Intentionally empty translation unit anchor.
 */
    .text
    .align 2
    .global _asm_parse_anchor
_asm_parse_anchor:
    ret
