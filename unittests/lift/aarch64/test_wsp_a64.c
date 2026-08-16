__attribute__((naked, noinline)) unsigned long test_wsp_zero_extend(void) {
  __asm__ volatile("add wsp, wsp, #0\n\t"
                   "mov x0, sp\n\t"
                   "ret");
}
