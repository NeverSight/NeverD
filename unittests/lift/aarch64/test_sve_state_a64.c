__attribute__((naked, noinline)) unsigned test_sve_last_byte(void) {
  __asm__ volatile("ptrue p0.b\n\t"
                   "mov z0.b, #1\n\t"
                   "lastb w0, p0, z0.b\n\t"
                   "ret");
}

__attribute__((naked, noinline)) unsigned test_sve_index_const_last(void) {
  __asm__ volatile("ptrue p0.b\n\t"
                   "index z0.b, #5, #0\n\t"
                   "lastb w0, p0, z0.b\n\t"
                   "ret");
}

__attribute__((naked, noinline)) unsigned long test_sve_index_step_last(void) {
  __asm__ volatile("ptrue p1.d\n\t"
                   "index z1.d, #7, #3\n\t"
                   "lastb x0, p1, z1.d\n\t"
                   "ret");
}
