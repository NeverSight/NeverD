//===- AllPlatform_RecursionCallRTTests.cpp - self-call / recursion -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// High-yield roundtrip probing of intra-module function CALL/RET, which every
// existing C-expression kernel sidesteps by being a leaf (clang lowers the
// builtins to instructions, never a call).  A self-recursive function forces a
// real `call`/`bl` to the function's own entry plus the caller-saved spill /
// callee-saved save-restore that surrounds it, exercising the lifter's call
// modelling, the loader's self-referential relocation, and the recompiled
// object's intra-.text branch fixup.  Depth is bounded by the argument's low
// bits so any input terminates; no 64-bit division is used (it would be a
// runtime-library call the bare-metal harness cannot resolve).  All four
// targets, compared native vs lifted.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64RecRT : public SemanticRoundTripFixture,
                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64RecRT, Verify) { roundTripX64(GetParam()); }
class X86RecRT : public SemanticRoundTripFixture,
                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86RecRT, Verify) { roundTripX86(GetParam()); }
class A64RecRT : public SemanticRoundTripFixture,
                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64RecRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32RecRT : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32RecRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeRecTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  std::string tree = p + "_tree", deep = p + "_deep", nest = p + "_nest";
  return {
    // Tree recursion: two self-calls per level with post-call mixing so neither
    // can be tail-folded.  The value read after each call must survive the
    // callee clobber, stressing caller-saved spill/reload around the call.
    {tree,
     t+" "+tree+"("+t+" a){\n"
     "  unsigned n=(unsigned)a; unsigned d=n&7u, v=n>>3;\n"
     "  if(d==0) return ("+t+")(unsigned long)(v*2654435761u+1u);\n"
     "  unsigned x=(unsigned)"+tree+"(("+t+")(((v+1u)<<3)|(d-1u)));\n"
     "  unsigned y=(unsigned)"+tree+"(("+t+")(((v^0x9E3779B9u)<<3)|((d-1u)>>1)));\n"
     "  return ("+t+")(unsigned long)((x*31u+y)^(x>>3)^(v*7u)); }\n",
     {0x1FULL}, "Rec", 2},

    // Linear deep recursion (depth ~24) with arithmetic both before and after
    // the call — exercises a tall call stack and the epilogue restore path.
    {deep,
     t+" "+deep+"("+t+" a){\n"
     "  unsigned n=(unsigned)a; unsigned d=n&31u, v=n>>5;\n"
     "  if(d==0) return ("+t+")(unsigned long)(v|1u);\n"
     "  unsigned pre=v*0x9E3779B1u + d;\n"
     "  unsigned r=(unsigned)"+deep+"(("+t+")(((pre)<<5)|(d-1u)));\n"
     "  return ("+t+")(unsigned long)((r^(r>>13))*0x85EBCA6Bu + pre); }\n",
     {0x3F8ULL}, "Rec", 2},

    // Nested (Ackermann-shaped) recursion: the second call's argument is itself
    // a recursive result, so the call graph and the data dependence interleave.
    {nest,
     t+" "+nest+"("+t+" a){\n"
     "  unsigned n=(unsigned)a; unsigned m=n&3u, k=(n>>2)&7u;\n"
     "  if(m==0) return ("+t+")(unsigned long)(k+1u);\n"
     "  if(k==0) return ("+t+")(unsigned long)"+nest+"(("+t+")(((1u)<<2)|(m-1u)));\n"
     "  unsigned inner=(unsigned)"+nest+"(("+t+")((((k-1u)&7u)<<2)|m));\n"
     "  return ("+t+")(unsigned long)"+nest+"(("+t+")(((inner&7u)<<2)|(m-1u))); }\n",
     {0xEULL}, "Rec", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeRecTC("x64rec", "long");
static const std::vector<RoundTripTC> kX86 = makeRecTC("x86rec", "int");
static const std::vector<RoundTripTC> kA64 = makeRecTC("a64rec", "long");
static const std::vector<RoundTripTC> kARM = makeRecTC("armrec", "int");

INSTANTIATE_TEST_SUITE_P(Rec, X64RecRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Rec, X86RecRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(Rec, A64RecRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Rec, ARM32RecRT, ::testing::ValuesIn(kARM), rtTCName);
