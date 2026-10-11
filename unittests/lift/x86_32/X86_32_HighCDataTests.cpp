//===- X86_32_HighCDataTests.cpp - i386 image data in HighC ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Position-independent i386 code reaches its data through the GOT.  Each
// kernel is decompiled to HighC and run on the host against its own C.
//
//===----------------------------------------------------------------------===//

#include "HighCHostExecution.h"

class X86_32_HighCData : public HighCHostExecutionTest {};

// Clang keeps the initializer of a local constant array in .rodata under an
// assembler-local label, so no symbol sizes the table the loop indexes
// through `.rodata@GOTOFF(%ebx,%eax,8)`.  The table is declared all the same,
// as the bytes up to the end of its section.
TEST_F(X86_32_HighCData, AnUnnamedTableIndexedThroughTheGotIsDeclared) {
  expectHighCRunsLikeSource({"-target", "i386-linux-gnu", "-march=pentium4"},
                            R"C(
int mix(int a) {
  unsigned long long s[10] = {0x9E3779B97F4A7C15ull, 0xBF58476D1CE4E5B9ull,
                              0x94D049BB133111EBull, 0x2545F4914F6CDD1Dull,
                              0xD6E8FEB86659FD93ull, 0xA0761D6478BD642Full,
                              0xE7037ED1A0B428DBull, 0x8EBC6AF09C88C6E3ull,
                              0x589965CC75374CC3ull, 0x1D8E4E27C47D124Full};
  unsigned h = (unsigned)a;
  for (int i = 0; i < 10; i++) {
    unsigned long long x = s[(i + (unsigned)a) % 10u];
    h = h * 131u + (unsigned)(x ^ (x >> 29));
  }
  return (int)h;
}
)C",
                            "mix", {"mix"}, {-3, 0, 1, 7, 77, 0x12345});
}
