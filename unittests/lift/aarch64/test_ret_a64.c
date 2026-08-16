__attribute__((naked, noinline, used)) unsigned ret_via_x16(void) {
  __asm__ volatile("adr x16, 1f\n\t"
                   "ret x16\n\t"
                   "1:\n\t"
                   "mov w0, #7\n\t"
                   "ret");
}
