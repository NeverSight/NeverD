__attribute__((naked, noinline, used)) void sve_store_ones(unsigned char *out) {
  __asm__ volatile("ptrue p0.b\n\t"
                   "mov z0.b, #1\n\t"
                   "st1b { z0.b }, p0, [x0]\n\t"
                   "ret");
}
