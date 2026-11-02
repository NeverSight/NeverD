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

#include <fstream>
#include <iterator>
#include <regex>
#include <string>

class X86_32_HighCData : public HighCHostExecutionTest {};

// Clang keeps the initializer of a local constant array in .rodata under an
// assembler-local label, so no symbol sizes the table the loop indexes
// through `.rodata@GOTOFF(%ebx,%eax,8)`.  The table is declared all the same,
// as the bytes up to the end of its section.
TEST_F(X86_32_HighCData, AnUnnamedTableIndexedThroughTheGotIsDeclared) {
  // The C computes the table's 32-bit image addresses.
  HostFlags = {"-m32"};
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

// `movw %bx, 8(%esp)` stores the low half of a value the function computed
// in EBX, a callee-saved register, into a local array.  The view is version 0
// of its own register, like an entry value, but no save of one: the store
// stays, and a later copy of the element reads it.
TEST_F(X86_32_HighCData, AViewOfACalleeSavedRegisterStoredInALocalStays) {
  // The C computes 32-bit frame addresses.
  HostFlags = {"-m32"};
  expectHighCRunsLikeSource({"-target", "i386-linux-gnu", "-march=pentium4"},
                            R"C(
struct ag { unsigned x; unsigned short y; unsigned char z; };
int agg(int a) {
  unsigned s = (unsigned)a | 1u;
  struct ag arr[5];
  unsigned long long acc = 0;
  for (int i = 0; i < 5; i++) {
    arr[i].x = s * ((unsigned)i + 1u);
    arr[i].y = (unsigned short)(s >> i);
    arr[i].z = (unsigned char)(s >> (8 + i));
  }
  for (int i = 0; i < 160; i++) {
    s = s * 1103515245u + 12345u;
    unsigned d = (s >> 4) % 5u, src = (s >> 8) % 5u;
    arr[d] = arr[src];
    arr[d].x += s;
    arr[d].y ^= (unsigned short)i;
    acc += (unsigned long long)arr[d].x + arr[d].y + arr[d].z;
    acc ^= acc >> 5;
  }
  return (int)(acc + arr[0].x);
}
)C",
                            "agg", {"agg"}, {-3, 4, 32, 39, 53, 60, 74});
}

// The double MULSD multiplies by is a constant-pool entry the code reaches
// through the GOT register, `mulsd .LCPI0_0@GOTOFF(%ebx)`: the C reads all
// eight of its bytes, not a two-byte placeholder for an unsized label.
TEST_F(X86_32_HighCData, AConstantMulsdReadsThroughTheGotIsWhole) {
  // The C computes 32-bit frame addresses and keeps SSE operands in xmm.
  HostFlags = {"-m32", "-march=pentium4"};
  expectHighCRunsLikeSource({"-target", "i386-linux-gnu", "-march=pentium4"},
                            R"C(
static int fi(int) __attribute__((noinline));
int intacc_fp(int x) {
  unsigned h = (unsigned)x, acc = 0;
  for (int i = 0; i < 120; i++) {
    h = h * 1103515245u + 12345u;
    double scratch = (double)((h >> 6) & 0xff) * 1.25;
    int r = fi((int)scratch);
    acc = acc * 131u + (unsigned)r + (unsigned)(int)scratch + (unsigned)i;
  }
  return (int)acc;
}
static int fi(int v) { return v * 11 + 5; }
)C",
                            "intacc_fp", {"intacc_fp", "fi"},
                            {-3, 0, 4, 11, 77});
}

// `SA[i - 1]` folds the -1 into the GOT-relative displacement, which then
// names the byte before SA: the last byte of the code before SA's section.
// It is SA's address less one, whatever lies at the byte itself.
TEST_F(X86_32_HighCData, AnIndexBiasBeforeAStringReadsTheString) {
  // The C keeps string addresses in 32-bit frame slots.
  HostFlags = {"-m32"};
  expectHighCRunsLikeSource({"-target", "i386-linux-gnu", "-march=pentium4"},
                            R"C(
static const char SA[] = "kitten_sequence_alpha";
static const char SB[] = "sitting_sequins_alfa";
int editdist(int a) {
  unsigned s = (unsigned)a, h = 0;
  const int LA = (int)(sizeof(SA) - 1), LB = (int)(sizeof(SB) - 1);
  for (int it = 0; it < 8; it++) {
    s = s * 1103515245u + 12345u;
    unsigned off = (s >> 7) & 3u, row[24];
    for (int j = 0; j <= LB; j++)
      row[j] = (unsigned)j;
    for (int i = 1; i <= LA; i++) {
      unsigned prev = row[0];
      row[0] = (unsigned)i;
      for (int j = 1; j <= LB; j++) {
        unsigned cur = row[j];
        unsigned cost = ((SA[i - 1] + off) == (unsigned)SB[j - 1]) ? 0u : 1u;
        unsigned m = prev + cost, d = row[j - 1] + 1u;
        if (d < m)
          m = d;
        d = row[j] + 1u;
        if (d < m)
          m = d;
        row[j] = m;
        prev = cur;
      }
    }
    h = h * 131u + row[LB] + off;
  }
  return (int)h;
}
)C",
                            "editdist", {"editdist", "SA", "SB"},
                            {-3, 0, 1, 0x53, 77});
}

// `movdqa` faults on a misaligned address, so the lift checks the address of
// the constant it reads.  That address is the constant's object, as the read
// is: its first int, 9, and the zero bytes after it also read as the UTF-16
// string "\t", a literal C would place elsewhere.  The object is declared as
// aligned as the image places it.
TEST_F(X86_32_HighCData, AnAlignedVectorReadChecksItsConstantsObject) {
  HostFlags = {"-m32", "-msse2"};
  expectHighCRunsLikeSource({"-target", "i386-linux-gnu", "-march=pentium4"},
                            R"C(
typedef unsigned v4u __attribute__((vector_size(16)));
int scale(int a) {
  v4u x = {(unsigned)a, (unsigned)a * 3u, (unsigned)a ^ 5u, (unsigned)a + 7u};
  v4u v = x * (v4u){9u, 10u, 11u, 12u};
  return (int)(v[0] ^ v[1] ^ v[2] ^ v[3]);
}
)C",
                            "scale", {"scale"}, {-3, 0, 1, 7, 77, 0x12345});
  if (HasFatalFailure() || IsSkipped())
    return;
  std::ifstream Input(tmpFile("decompiled_high.c"));
  const std::string C((std::istreambuf_iterator<char>(Input)),
                      std::istreambuf_iterator<char>());
  std::smatch Checked;
  ASSERT_TRUE(
      std::regex_search(C, Checked, std::regex(R"(\(&(\w+)\)\) & 15\) != 0)")))
      << C;
  EXPECT_TRUE(std::regex_search(
      C, std::regex("_Alignas\\(16\\) [^;\\n]*\\b" + Checked[1].str() + " =")))
      << C;
  EXPECT_EQ(C.find("L\""), std::string::npos) << C;
}
