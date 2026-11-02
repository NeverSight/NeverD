//===- parameter_bound_progress_fixture.c - Owned input-bound loops -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Unknown initial and final counters, odd/even steps and body/exit patches.
/// Compile and inspect only; negative controls may run forever.
//===----------------------------------------------------------------------===//
#ifndef COUNT_STEP
#define COUNT_STEP 3u
#endif
#ifndef EXIT_OFFSET
#define EXIT_OFFSET 0u
#endif
#ifndef LOOP_MUL
#define LOOP_MUL 7u
#endif
#ifndef FINAL_ADD
#define FINAL_ADD 3u
#endif
__attribute__((noinline, used)) unsigned transform(unsigned x, unsigned limit) {
  unsigned n = x;
  do {
    if (x & 1u) {
      x = (x ^ 23u) * LOOP_MUL + 9u;
      x = (x >> 5) | (x << 27);
    } else {
      x = (x + 17u) ^ (n * 13u);
      x ^= (x << 7) + limit;
    }
    n += COUNT_STEP;
  } while (n != limit + EXIT_OFFSET);
  return (x ^ limit) + FINAL_ADD;
}
