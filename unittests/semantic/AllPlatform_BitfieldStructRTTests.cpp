//===- AllPlatform_BitfieldStructRTTests.cpp - bitfield rmw ----*- C++ -*-===//
//
// clang -O2 C bitfield kernels across all four targets.  Bitfield writes lower
// to a load / clear-field-mask / shift-insert / or-store read-modify-write, and
// reads to a shift + mask (signed fields add a sign-extend from the field
// width).  Mixed/odd widths cross byte boundaries, so the recompiled code must
// reproduce the exact mask/shift/sign-extend the original used or a field's
// value (and the folded accumulator) diverges.  64-bit-wide bitfield containers
// additionally exercise the i386/ARM32 register-pair shift/mask path.  Each
// kernel folds to an exact integer return.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64BitfieldRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64BitfieldRT, Verify) { roundTripX64(GetParam()); }

class X86BitfieldRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86BitfieldRT, Verify) { roundTripX86(GetParam()); }

class A64BitfieldRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64BitfieldRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32BitfieldRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32BitfieldRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeBitfield(const char *prefix) {
  std::string p = prefix;
  return {
    // 32-bit container, mixed unsigned widths crossing byte boundaries.
    {p+"_u32",
     "int "+p+"_u32(int seed){\n"
     "  struct B{unsigned a:3,b:5,c:7,d:9,e:8;} s={0,0,0,0,0}; int acc=0;\n"
     "  for(int i=0;i<128;i++){\n"
     "    s.a=(unsigned)(seed+i); s.b=(unsigned)(seed*i)>>2;\n"
     "    s.c=(unsigned)(seed^i); s.d=(unsigned)(i*131+seed);\n"
     "    s.e+=(unsigned)(seed+i);\n"
     "    acc += (int)s.a + (int)s.b*2 - (int)s.c + (int)s.d - (int)s.e; }\n"
     "  return acc; }\n",
     {0x1234567ULL}, "Bitfield", 2, ""},

    // Signed bitfields: each read sign-extends from its field width.
    {p+"_signed",
     "int "+p+"_signed(int seed){\n"
     "  struct B{int a:4; int b:11; int c:6; int d:9; unsigned f:2;} s={0,0,0,0,0};\n"
     "  int acc=0;\n"
     "  for(int i=0;i<128;i++){\n"
     "    s.a=seed+i; s.b=seed*i-i*7; s.c=(seed^i)-i; s.d=i*131-seed;\n"
     "    s.f=(unsigned)(seed+i);\n"
     "    acc += s.a - s.b + s.c*3 - s.d + (int)s.f; }\n"
     "  return acc; }\n",
     {0x2233445ULL}, "Bitfield", 2, ""},

    // 64-bit container: wide fields exercise register-pair mask/shift on 32-bit.
    {p+"_u64",
     "int "+p+"_u64(int seed){\n"
     "  struct B{unsigned long long a:5,b:20,c:17,d:13,e:9;} s={0,0,0,0,0};\n"
     "  unsigned long long acc=0;\n"
     "  for(int i=0;i<120;i++){\n"
     "    s.a=(unsigned)(seed+i); s.b=(unsigned long long)(seed*i)*2654435761ull>>10;\n"
     "    s.c=(unsigned)(seed^(i*131)); s.d+=(unsigned)(seed+i*7);\n"
     "    s.e=(unsigned)(i-seed);\n"
     "    acc += s.a + s.b - s.c + s.d*2 - s.e; }\n"
     "  return (int)(acc ^ (acc>>32)); }\n",
     {0x3344556ULL}, "Bitfield", 2, ""},

    // Toggling/accumulating in place (load-modify-store on the same field).
    {p+"_rmw",
     "int "+p+"_rmw(int seed){\n"
     "  struct B{unsigned lo:12, mid:11, hi:9;} s={1,2,3}; int acc=0;\n"
     "  for(int i=0;i<150;i++){\n"
     "    s.lo += (unsigned)(seed+i); s.mid ^= (unsigned)(seed*i);\n"
     "    s.hi -= (unsigned)(i); if((s.lo&1u)) s.hi += s.mid;\n"
     "    acc += (int)s.lo - (int)s.mid + (int)s.hi; }\n"
     "  return acc; }\n",
     {0x4455667ULL}, "Bitfield", 2, ""},
  };
}

static const std::vector<RoundTripTC> kX64Bf   = makeBitfield("x64bf");
static const std::vector<RoundTripTC> kX86Bf   = makeBitfield("x86bf");
static const std::vector<RoundTripTC> kA64Bf   = makeBitfield("a64bf");
static const std::vector<RoundTripTC> kARM32Bf = makeBitfield("armbf");
// clang-format on

INSTANTIATE_TEST_SUITE_P(Bitfield, X64BitfieldRT, ::testing::ValuesIn(kX64Bf),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(Bitfield, X86BitfieldRT, ::testing::ValuesIn(kX86Bf),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(Bitfield, A64BitfieldRT, ::testing::ValuesIn(kA64Bf),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(Bitfield, ARM32BitfieldRT, ::testing::ValuesIn(kARM32Bf),
                         rtTCName);
