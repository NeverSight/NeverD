__attribute__((naked, noinline)) unsigned long
test_sve_length_a64(unsigned long value) {
  __asm__ volatile("sub sp, sp, #16\n\t"
                   "cntb x8\n\t"
                   "incb x0\n\t"
                   "add x0, x8, x0\n\t"
                   "str x0, [sp]\n\t"
                   "ldr x0, [sp]\n\t"
                   "add sp, sp, #16\n\t"
                   "ret");
}
