__attribute__((noinline)) unsigned test_bc_eq(unsigned value) {
  unsigned result;
  __asm__ volatile("cmp %w1, #0\n\t"
                   "bc.eq 1f\n\t"
                   "mov %w0, #0\n\t"
                   "b 2f\n"
                   "1:\n\t"
                   "mov %w0, #1\n"
                   "2:"
                   : "=r"(result)
                   : "r"(value)
                   : "cc");
  return result;
}
