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

__attribute__((naked, noinline)) unsigned long test_sve_incb_mul2(void) {
  __asm__ volatile("mov x0, #10\n\t"
                   "incb x0, all, mul #2\n\t"
                   "ret");
}

__attribute__((naked, noinline)) unsigned long test_sve_decw_mul4(void) {
  __asm__ volatile("mov x0, #100\n\t"
                   "decw x0, all, mul #4\n\t"
                   "ret");
}

__attribute__((naked, noinline)) unsigned long test_sve_addvl_two(void) {
  __asm__ volatile("mov x0, #10\n\t"
                   "addvl x0, x0, #2\n\t"
                   "ret");
}

__attribute__((naked, noinline)) unsigned long test_sve_addvl_negative(void) {
  __asm__ volatile("mov x0, #100\n\t"
                   "addvl x0, x0, #-3\n\t"
                   "ret");
}
