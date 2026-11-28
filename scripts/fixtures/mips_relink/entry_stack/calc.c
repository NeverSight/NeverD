__attribute__((section(".text.calc"), noinline)) unsigned calc(unsigned value) {
  return value + BIAS;
}
