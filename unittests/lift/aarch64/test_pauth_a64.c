__attribute__((noinline)) unsigned long
test_pacga_a64(unsigned long value, unsigned long discriminator) {
  unsigned long result;
  __asm__ volatile("pacga %0, %1, %2"
                   : "=r"(result)
                   : "r"(value), "r"(discriminator));
  return result;
}
