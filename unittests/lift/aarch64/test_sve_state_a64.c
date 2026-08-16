__attribute__((naked, noinline)) unsigned test_sve_last_byte(void) {
  __asm__ volatile("ptrue p0.b\n\t"
                   "mov z0.b, #1\n\t"
                   "lastb w0, p0, z0.b\n\t"
                   "ret");
}
