//===- modular_progress_fixture.c - Owned wrapping counter loops ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Unknown initial counters, odd/even steps and reachable/unreachable exits.
/// Compile and inspect only; negative controls may run forever.
//===----------------------------------------------------------------------===//
#ifndef COUNT_STEP
#define COUNT_STEP 3u
#endif
#ifndef EXIT_WIDTH
#define EXIT_WIDTH 1u
#endif
#ifndef LOOP_MUL
#define LOOP_MUL 7u
#endif
#ifndef FINAL_ADD
#define FINAL_ADD 3u
#endif
__attribute__((noinline, used)) unsigned transform(unsigned x, unsigned y) {
  unsigned n = y;
  do {
    if (x & 1u) {
      x = (x ^ 23u) * LOOP_MUL + 9u;
      x = (x >> 5) | (x << 27);
    } else {
      x = (x + 17u) ^ (n * 13u);
      x ^= (x << 7) + y;
    }
    n += COUNT_STEP;
  } while (n >= EXIT_WIDTH);
  return (x ^ y) + FINAL_ADD;
}
