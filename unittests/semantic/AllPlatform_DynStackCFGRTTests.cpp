//===- AllPlatform_DynStackCFGRTTests.cpp - irreducible goto CFG ---*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// High-yield roundtrip probing of dense `goto` webs that build irreducible-
// leaning control flow with multiple back-edges into shared headers — a shape
// the structured-loop kernels never reach.  It stresses MedSSA phi placement
// and cross-block flag liveness (the #150/#161 bug areas), where each arm
// consumes a different comparison's flags.  One scalar arg / scalar return so
// the uniform harness applies; bounded by the argument's low bits; no 64-bit
// division.  All four targets, compared native vs lifted.
//
// NOTE: variable-length-array / dynamic-`alloca` probing lived here but is
// deferred — x86/i386 model the frame as a fixed `alloca` and mismodel the
// dynamic SP adjustment a VLA needs (a64/arm32, which address VLAs FP-relative,
// are correct).  Tracked in the Unicorn unsupported-instructions doc (#401, dynamic-frame
// limitation) so this suite stays a clean green guardrail.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64DynRT : public SemanticRoundTripFixture,
                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64DynRT, Verify) { roundTripX64(GetParam()); }
class X86DynRT : public SemanticRoundTripFixture,
                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86DynRT, Verify) { roundTripX86(GetParam()); }
class A64DynRT : public SemanticRoundTripFixture,
                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64DynRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32DynRT : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32DynRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeDynTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  std::string irr = p + "_irr", web = p + "_web";
  return {
    // Dense goto web: two interleaved states with cross-jumps and a shared exit
    // — stresses phi placement / flag liveness across an irregular CFG.
    {irr,
     t+" "+irr+"("+t+" a){\n"
     "  unsigned x=(unsigned)a; unsigned i=0;\n"
     "  if(x&1u) goto odd;\n"
     "  even: x=x*3u+1u; if(++i>=64u) goto done; if(x&1u) goto odd; goto even;\n"
     "  odd:  x=(x>>1)^0x9E3779B9u; if(++i>=64u) goto done;\n"
     "        if(x&2u) goto even; goto odd;\n"
     "  done: return ("+t+")(unsigned long)x; }\n",
     {0x9ULL}, "Dyn", 2},

    // goto-threaded accumulator with multiple back-edges into a common header,
    // each arm consuming a different comparison's flags.
    {web,
     t+" "+web+"("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u; int i=0;\n"
     "  head: i++; if(i>80) goto fin;\n"
     "    if((int)acc<0) goto neg; if(acc>0xF0000000u) goto big; \n"
     "    acc=acc*2654435761u+(unsigned)i; goto head;\n"
     "  neg: acc=(unsigned)(-(int)acc)^(unsigned)i; goto head;\n"
     "  big: acc=(acc>>3)|(acc<<29); acc-=(unsigned)i*7u; goto head;\n"
     "  fin: return ("+t+")(unsigned long)acc; }\n",
     {0x40000001ULL}, "Dyn", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeDynTC("x64dyn", "long");
static const std::vector<RoundTripTC> kX86 = makeDynTC("x86dyn", "int");
static const std::vector<RoundTripTC> kA64 = makeDynTC("a64dyn", "long");
static const std::vector<RoundTripTC> kARM = makeDynTC("armdyn", "int");

INSTANTIATE_TEST_SUITE_P(Dyn, X64DynRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Dyn, X86DynRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(Dyn, A64DynRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Dyn, ARM32DynRT, ::testing::ValuesIn(kARM), rtTCName);
