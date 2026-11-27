/* The fixture is linked and inspected as data. */
__attribute__((noinline)) int first(void) {
  volatile unsigned value;
  return value != 0;
}

__attribute__((noinline)) int second(void) {
  volatile unsigned value = 17;
  return value != 0;
}
