/* Owned source-grounded comparison fixture; never executed by diff tests. */
#ifndef PATCH_LIMIT
#define PATCH_LIMIT 17
#endif

__attribute__((noinline)) int stable(int value) {
  return (value * 13 + 7) ^ 0x139;
}

__attribute__((noinline)) int checked(unsigned value) {
  if (value >= PATCH_LIMIT)
    return -1;
  return stable((int)value);
}

__attribute__((noinline)) int entry(unsigned value) {
  return checked(value) + stable((int)(value >> 2));
}
