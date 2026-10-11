//===- X86_32_HighCSignednessTests.cpp - Signedness of HighC operands -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A comparison, division or shift needs operands of one signedness, which C
// decides from the operands' printed types.  Each kernel is decompiled to
// HighC and run on the host against its own C.
//
//===----------------------------------------------------------------------===//

#include "HighCHostExecution.h"

class X86_32_HighCSignedness : public HighCHostExecutionTest {};

// The loop compares the same values signed and unsigned.  The writer prints
// an XOR in place of the variable it defines, and C makes the XOR of an int
// and an unsigned unsigned, so the signed comparison must cast it.
TEST_F(X86_32_HighCSignedness, ForwardedArithmeticKeepsItsComparison) {
  expectHighCRunsLikeSource({"-target", "i386-linux-gnu", "-march=pentium4"},
                            R"C(
int cmppolar(int a) {
  unsigned x = (unsigned)a, h = 0;
  for (int i = 0; i < 64; i++) {
    unsigned y = x ^ ((unsigned)i * 0x9E3779B9u);
    int su = (int)x < (int)y;
    int uu = x < y;
    int sge = (int)x >= (int)y;
    int uge = x >= y;
    h += (unsigned)su * 131u + (unsigned)uu * 7u + (unsigned)sge * 5u +
         (unsigned)uge * 3u;
    if (su != uu)
      h ^= 0xABCDu;
    x = (x * 1664525u + 1013904223u) ^ (y + h);
  }
  return (int)h;
}
)C",
                            "cmppolar", {"cmppolar"},
                            {0x55AA, 0, 1, -1, 0x12345678, 0xA5, 0x80});
}
