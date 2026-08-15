/* AArch64 FEAT_FAMINMAX absolute floating-point min/max probes. */

__attribute__((naked, noinline)) unsigned long
famax_fp16_bits(unsigned long a, unsigned long b) {
  __asm__ volatile("fmov d0, x0\n\t"
                   "fmov d1, x1\n\t"
                   "famax v2.4h, v0.4h, v1.4h\n\t"
                   "fmov x0, d2\n\t"
                   "ret"
                   :
                   :
                   : "x0", "v0", "v1", "v2");
}

__attribute__((naked, noinline)) unsigned long
famin_fp16_bits(unsigned long a, unsigned long b) {
  __asm__ volatile("fmov d0, x0\n\t"
                   "fmov d1, x1\n\t"
                   "famin v2.4h, v0.4h, v1.4h\n\t"
                   "fmov x0, d2\n\t"
                   "ret"
                   :
                   :
                   : "x0", "v0", "v1", "v2");
}
