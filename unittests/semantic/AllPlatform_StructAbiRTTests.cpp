//===- AllPlatform_StructAbiRTTests.cpp - struct-by-value ABI ---*-C++*-=//
//
// The call-ABI probes so far passed scalars, two small all-integer structs and a
// register-pair struct return.  These probes cross the call boundary with the
// struct shapes that take *distinct* ABI paths the earlier tests never reached:
//
//   * hfa2f / hfa4f / hfa2d - homogeneous floating aggregates.  AArch64 passes a
//     struct of 2-4 floats/doubles in consecutive V registers (V0..V3), not the
//     integer registers; x86-64 packs {float,float} into one XMM and {double,
//     double} into two; i386/arm32 spill to the stack.  A lifter that models a
//     struct argument as integer lanes mis-routes every field.
//   * mixfp  - a struct mixing an int and floats: not an HFA, so it falls back to
//     the integer/memory path even on AArch64 (the HFA test's negative control).
//   * big24  - a 24-byte (six-int) struct passed by value: larger than the
//     register budget, so it is passed in memory (x86-64 SysV) or by reference
//     (AArch64 indirect), exercising large-aggregate argument lowering.
//   * sret24 - a 24-byte struct returned by value through the hidden sret pointer
//     (x8 on AArch64, a leading pointer arg on x86-64 with the address echoed in
//     rax), exercising the indirect-return ABI.
//
// Every kernel folds its result to a single integer return so native and lifted
// emulation compare a scalar.  All four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

#include <algorithm>

class X64StructAbiRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64StructAbiRT, Verify) { roundTripX64(GetParam()); }
class X86StructAbiRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86StructAbiRT, Verify) { roundTripX86(GetParam()); }
class A64StructAbiRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64StructAbiRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32StructAbiRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32StructAbiRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeStructAbiTC(const char *prefix, const char *T,
                                                const char *flags) {
  std::string p = prefix, t = T, f = flags;
  return {
    // Homogeneous aggregate of two floats (AArch64: V0/V1 ; x86-64: one XMM).
    {p+"_hfa2f",
     "struct "+p+"F2{ float x,y; };\n"
     "static float "+p+"_h2f(struct "+p+"F2) __attribute__((noinline));\n"
     +t+" "+p+"_hfa2f("+t+" a){\n"
     "  float v=(float)((unsigned)a&0x3ffu);\n"
     "  struct "+p+"F2 s={v+1.5f, v*0.5f+2.0f};\n"
     "  return ("+t+")(int)"+p+"_h2f(s); }\n"
     "static float "+p+"_h2f(struct "+p+"F2 s){ return s.x*s.y + s.x - s.y; }\n",
     {0x41ULL}, "StructAbi", 2, f},

    // Homogeneous aggregate of four floats (AArch64: V0-V3).
    {p+"_hfa4f",
     "struct "+p+"F4{ float a,b,c,d; };\n"
     "static float "+p+"_h4f(struct "+p+"F4) __attribute__((noinline));\n"
     +t+" "+p+"_hfa4f("+t+" a){\n"
     "  float v=(float)((unsigned)a&0x3ffu);\n"
     "  struct "+p+"F4 s={v+1.0f, v*0.5f, v-3.0f, v*0.25f+1.0f};\n"
     "  return ("+t+")(int)"+p+"_h4f(s); }\n"
     "static float "+p+"_h4f(struct "+p+"F4 s){ return s.a*s.b + s.c*s.d; }\n",
     {0x9bULL}, "StructAbi", 2, f},

    // Homogeneous aggregate of two doubles (AArch64: V0/V1 ; x86-64: XMM0/XMM1).
    {p+"_hfa2d",
     "struct "+p+"D2{ double x,y; };\n"
     "static double "+p+"_h2d(struct "+p+"D2) __attribute__((noinline));\n"
     +t+" "+p+"_hfa2d("+t+" a){\n"
     "  double v=(double)((unsigned)a&0x3ffu)+1.0;\n"
     "  struct "+p+"D2 s={v*1.25, v*0.5+2.0};\n"
     "  return ("+t+")(int)"+p+"_h2d(s); }\n"
     "static double "+p+"_h2d(struct "+p+"D2 s){ return s.x*s.y + s.x/s.y; }\n",
     {0xa7ULL}, "StructAbi", 2, f},

    // Mixed int + float struct (not an HFA): integer/memory path even on AArch64.
    {p+"_mixfp",
     "struct "+p+"MF{ float a; int b; float c; };\n"
     "static float "+p+"_hmf(struct "+p+"MF) __attribute__((noinline));\n"
     +t+" "+p+"_mixfp("+t+" a){\n"
     "  unsigned u=(unsigned)a; float v=(float)(u&0x3ffu);\n"
     "  struct "+p+"MF s={v+0.5f, (int)(u&7u), v*2.0f-1.0f};\n"
     "  return ("+t+")(int)"+p+"_hmf(s); }\n"
     "static float "+p+"_hmf(struct "+p+"MF s){ return s.a*s.c + (float)s.b; }\n",
     {0x35ULL}, "StructAbi", 2, f},

    // 24-byte (six-int) struct passed by value: memory / by-reference path.
    {p+"_big24",
     "struct "+p+"B6{ unsigned a,b,c,d,e,g; };\n"
     "static unsigned "+p+"_hb6(struct "+p+"B6) __attribute__((noinline));\n"
     +t+" "+p+"_big24("+t+" a){\n"
     "  unsigned v=(unsigned)a;\n"
     "  struct "+p+"B6 s={v,v*3u+1u,v^0x55u,v+0x1000u,v*7u,v|0x80000000u};\n"
     "  return ("+t+")(unsigned long)"+p+"_hb6(s); }\n"
     "static unsigned "+p+"_hb6(struct "+p+"B6 s){\n"
     "  return (((((s.a*31u+s.b)*31u+s.c)*31u+s.d)*31u+s.e)*31u+s.g); }\n",
     {0x6dULL}, "StructAbi", 2, f},

    // 24-byte struct returned by value through the hidden sret pointer.
    {p+"_sret24",
     "struct "+p+"R6{ unsigned a,b,c,d,e,g; };\n"
     "static struct "+p+"R6 "+p+"_mk6(unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_sret24("+t+" a){\n"
     "  struct "+p+"R6 r="+p+"_mk6((unsigned)a);\n"
     "  return ("+t+")(unsigned long)\n"
     "    (((((r.a*31u+r.b)*31u+r.c)*31u+r.d)*31u+r.e)*31u+r.g); }\n"
     "static struct "+p+"R6 "+p+"_mk6(unsigned x){\n"
     "  struct "+p+"R6 r; r.a=x; r.b=x*3u+1u; r.c=x^0x55u;\n"
     "  r.d=x+0x1000u; r.e=x*7u; r.g=x|0x80000000u; return r; }\n",
     {0x13ULL}, "StructAbi", 2, f},
  };
}
// clang-format on

// Drop a named case (by base suffix) pending a dedicated fix.
static std::vector<RoundTripTC> dropCases(std::vector<RoundTripTC> V,
                                          std::vector<std::string> Suffixes) {
  V.erase(std::remove_if(V.begin(), V.end(),
                         [&](const RoundTripTC &TC) {
                           for (const auto &S : Suffixes)
                             if (TC.Name.size() >= S.size() &&
                                 TC.Name.compare(TC.Name.size() - S.size(),
                                                 S.size(), S) == 0)
                               return true;
                           return false;
                         }),
          V.end());
  return V;
}

static const std::vector<RoundTripTC> kX64 = makeStructAbiTC("x64sab", "long", "");
// i386 passes the 24-byte struct entirely on the stack; clang's struct copy
// #428 two-pass recoverCallAbi resolves the i386-regparm-ambiguity for cdecl
// callees (CalleeRegArgs == 0 after forwarder promotion) — big24 now passes.
static const std::vector<RoundTripTC> kX86 =
    makeStructAbiTC("x86sab", "int", "");
// AArch64 returns a >16-byte struct through the indirect-result register x8;
// #423 models it as a hidden trailing indirect-result pointer parameter so the
// callee writes the aggregate through it and the caller passes the result-buffer
// pointer it set up.
static const std::vector<RoundTripTC> kA64 = makeStructAbiTC("a64sab", "long", "");
static const std::vector<RoundTripTC> kARM =
    makeStructAbiTC("armsab", "int", "-mfloat-abi=softfp -mfpu=vfpv3 -fno-math-errno");

INSTANTIATE_TEST_SUITE_P(StructAbi, X64StructAbiRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(StructAbi, X86StructAbiRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(StructAbi, A64StructAbiRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(StructAbi, ARM32StructAbiRT, ::testing::ValuesIn(kARM), rtTCName);
