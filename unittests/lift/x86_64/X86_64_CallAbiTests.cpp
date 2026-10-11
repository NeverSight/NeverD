//===- X86_64_CallAbiTests.cpp - x86-64 call arguments in HighC -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// System V x86-64 passes the seventh and later integer arguments on the
// stack, and a callee can come from a stack slot.  Each kernel is decompiled
// to HighC and run on the host against its own C.
//
//===----------------------------------------------------------------------===//

#include "HighCHostExecution.h"

class X86_64_CallAbi : public HighCHostExecutionTest {};

// Six register arguments and four pushed afresh on every iteration of a
// loop, whose stack pointer is a PHI at the loop head.
TEST_F(X86_64_CallAbi, StackArgumentsPushedInALoopReachTheCallee) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
static unsigned ma(unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
                   unsigned, unsigned, unsigned, unsigned)
    __attribute__((noinline));
int manyarg(int a) {
  unsigned s = (unsigned)a, acc = 0x811C9DC5u;
  for (int k = 0; k < 40; k++) {
    unsigned r = ma(s, s ^ acc, s + (unsigned)k, acc, s * 3u, acc >> 2,
                    s ^ 0x9E3779B9u, acc * 7u, s + acc, (unsigned)k * 131u);
    acc = (acc ^ r) * 16777619u;
    s = s * 1664525u + 1013904223u;
  }
  return (int)acc;
}
static unsigned ma(unsigned a, unsigned b, unsigned c, unsigned d, unsigned e,
                   unsigned f, unsigned g, unsigned h, unsigned i,
                   unsigned j) {
  return ((a * 131u + b) ^ (c << 3)) + (d - e) + (f ^ g) + (h * 5u) - i +
         (j >> 1);
}
)C",
                            "manyarg", {"manyarg", "ma"}, {-3, 0, 1, 77});
}

// `cmove` picks which of two stack slots the tail call loads its callee
// from, so the stores that fill both slots are read through that choice.
TEST_F(X86_64_CallAbi, ACalleePickedFromTwoStackSlotsIsTheStoredOne) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
static int pick_p(int) __attribute__((noinline));
static int pick_q(int) __attribute__((noinline));
typedef int (*pick_fn)(int);
int pick(int a) {
  volatile pick_fn s0 = pick_p, s1 = pick_q;
  pick_fn fp = (a & 1) ? s1 : s0;
  return fp(a * 3 + 1);
}
static int pick_p(int x) {
  unsigned h = (unsigned)x;
  for (unsigned i = 0; i < 6; i++)
    h = h * 131u + i;
  return (int)h;
}
static int pick_q(int x) {
  return (int)(((unsigned)x * 2654435761u) ^ ((unsigned)x >> 7));
}
)C",
                            "pick", {"pick", "pick_p", "pick_q"},
                            {-3, 0, 1, 2, 77});
}
