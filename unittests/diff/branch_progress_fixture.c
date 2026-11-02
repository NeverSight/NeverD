//===- branch_progress_fixture.c - Owned guarded counter paths -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compile and inspect common and selected counter updates; never execute.
//===----------------------------------------------------------------------===//
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
#define FAMILY 5
#endif
__attribute__((noinline, used)) unsigned transform(unsigned x, unsigned y) {
#if FAMILY == 5
  unsigned n = (y & LOOP_MASK) + 1u;
#else
  int n = (int)(y & LOOP_MASK);
#endif
again:
  if (x & 1u) {
    x = (x ^ 23u) * LOOP_MUL + 9u;
    x = (x >> 5) | (x << 27);
#if FAMILY == 5
    if (--n)
#else
    if (--n > 0)
#endif
      goto again;
  } else {
    x = (x + 17u) ^ (y * 13u);
    x ^= (x << 7) + y;
#if FAMILY == 5
    if (--n)
#else
    if (--n > 0)
#endif
      goto again;
  }
  return (x ^ y) + FINAL_ADD;
}
