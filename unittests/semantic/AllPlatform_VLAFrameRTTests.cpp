//===- AllPlatform_VLAFrameRTTests.cpp - dynamic stack frames -----*-C++*-=//
//
// Roundtrip probing of variable-length arrays / dynamic `alloca`.  A VLA makes
// clang adjust the stack pointer by a runtime amount (`sub rsp, reg`) and
// address the array relative to the adjusted SP.  The emitter recognises that
// runtime SP decrement and lowers it to a real dynamic LLVM alloca, so the
// variable region stays in bounds instead of reading below the fixed-size frame
// alloca — the #401/#404 "x86/i386 dynamic frame" limitation, now closed on all
// four targets (i386 had no red zone to mask the out-of-bounds access).  Each
// kernel takes one scalar arg and returns a value-dependent hash; the roundtrip
// compares native vs lifted execution.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VlaRT : public SemanticRoundTripFixture,
                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VlaRT, Verify) { roundTripX64(GetParam()); }
class X86VlaRT : public SemanticRoundTripFixture,
                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86VlaRT, Verify) { roundTripX86(GetParam()); }
class A64VlaRT : public SemanticRoundTripFixture,
                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VlaRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32VlaRT : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VlaRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeVlaTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Single VLA: one runtime-sized array, written then reduced.
    {p+"_vla1",
     t+" "+p+"_vla1("+t+" a){\n"
     "  unsigned n=((unsigned)a&7u)+2u;\n"
     "  unsigned b[n];\n"
     "  unsigned h=0;\n"
     "  for(unsigned i=0;i<n;i++) b[i]=i*(unsigned)a+1u;\n"
     "  for(unsigned i=0;i<n;i++) h=h*31u+b[i];\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x35ULL}, "Vla", 2},

    // Two VLAs: independent runtime sizes laid out one below the other.
    {p+"_vla2",
     t+" "+p+"_vla2("+t+" a){\n"
     "  unsigned n=((unsigned)a&3u)+2u, m=(((unsigned)a>>4)&3u)+2u;\n"
     "  unsigned x[n], y[m];\n"
     "  unsigned h=0;\n"
     "  for(unsigned i=0;i<n;i++) x[i]=i*(unsigned)a+1u;\n"
     "  for(unsigned i=0;i<m;i++) y[i]=i*(unsigned)a*3u+7u;\n"
     "  for(unsigned i=0;i<n;i++) h=h*31u+x[i];\n"
     "  for(unsigned i=0;i<m;i++) h=h*131u+y[i];\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x35ULL}, "Vla", 2},

    // VLA plus a fixed local: the dynamic region coexists with fixed slots.
    {p+"_vlamix",
     t+" "+p+"_vlamix("+t+" a){\n"
     "  unsigned n=((unsigned)a&7u)+3u;\n"
     "  unsigned fixed[4]={1u,2u,3u,4u};\n"
     "  unsigned b[n];\n"
     "  unsigned h=(unsigned)a;\n"
     "  for(unsigned i=0;i<n;i++) b[i]=(i^(unsigned)a)*2654435761u;\n"
     "  for(unsigned i=0;i<4;i++) h+=fixed[i]*b[i%n];\n"
     "  for(unsigned i=0;i<n;i++) h=h*31u+b[i];\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x35ULL}, "Vla", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeVlaTC("x64vla", "long");
static const std::vector<RoundTripTC> kX86 = makeVlaTC("x86vla", "int");
static const std::vector<RoundTripTC> kA64 = makeVlaTC("a64vla", "long");
static const std::vector<RoundTripTC> kARM = makeVlaTC("armvla", "int");

INSTANTIATE_TEST_SUITE_P(Vla, X64VlaRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Vla, X86VlaRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(Vla, A64VlaRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Vla, ARM32VlaRT, ::testing::ValuesIn(kARM), rtTCName);
