/* Owned control-flow comparison fixture. Compile and inspect; never execute. */
#ifndef FINAL_ADD
#define FINAL_ADD 3u
#endif
#ifndef LOOP_MASK
#define LOOP_MASK 7u
#endif
#ifndef LOOP_MUL
#define LOOP_MUL 7u
#endif
#ifndef FAMILY
#define FAMILY 1
#endif
#ifdef HIGH_INPUT
__attribute__((noinline, used)) unsigned transform(unsigned x,
                                                   unsigned long long raw_y) {
  unsigned y = (unsigned)raw_y;
#else
__attribute__((noinline, used)) unsigned transform(unsigned x, unsigned y) {
#endif
#if FAMILY == 1
#ifdef CHANGED_BRANCH
  if (y < x)
#else
  if (x < y)
#endif
    x = (x ^ 23u) * 7u + 9u;
  else
    x = (x + 17u) ^ (y * 13u);
#else
  unsigned n = y & LOOP_MASK;
  while (n--) {
#if FAMILY == 4
    unsigned m = (x & 7u) + 1u;
    while (m--) {
      x = (x ^ 23u) * LOOP_MUL + 9u;
      x = (x >> 5) | (x << 27);
      x = (x + n) ^ y;
    }
    x ^= (x << 7) + n;
#elif FAMILY == 3
    if (x & 1u) {
      x = (x ^ 23u) * LOOP_MUL + 9u;
      x = (x >> 5) | (x << 27);
      x = (x + 0x1234567u) ^ y;
      x = (x ^ (x >> 11)) * 13u;
      x = (x << 7) | (x >> 25);
    } else {
      x = (x + 17u) ^ (y * 13u);
    }
#else
    x = (x ^ 23u) * LOOP_MUL + 9u;
    x = (x >> 5) | (x << 27);
#endif
  }
#endif
#ifdef HIGH_INPUT
  return ((x ^ y) + FINAL_ADD) ^ (unsigned)(raw_y >> 32);
#else
  return (x ^ y) + FINAL_ADD;
#endif
}
