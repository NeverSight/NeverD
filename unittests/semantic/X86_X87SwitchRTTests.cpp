//===- X86_X87SwitchRTTests.cpp - x87 stack across switch tables -*-C++*-=//
//
// Stresses the CFG-order x87 TOP propagation (the #451 fixupFpuStack) through a
// jump-table dispatch: a `switch` lowers to an indirect branch (INDIR_BR) whose
// many successors each operate on a `long double` left resident on the x87 stack.
// Unlike the two-way branch case, the table successors are reached via the
// resolved jump-table edges, so this confirms the TOP dataflow propagates across
// INDIR_BR successors as well.  The result is read from the low 64 mantissa
// bytes of the stored f80, native vs lifted.
//
//   * swacc   - switch picks an operation on a loop-carried f80 accumulator
//   * swdense - a denser switch with default + fallthrough-free arms
//
// (A third kernel keeping two f80 values resident across a switch whose arms
// reach a *deeper* shared tail surfaced a distinct, pre-existing deep x87-SSA
// reaching-def defect on i386 -- see the Unicorn unsupported-instructions doc #451 -- and is
// tracked there as the next x87 target rather than shipped red.)
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64X87SwitchRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64X87SwitchRT, Verify) { roundTripX64(GetParam()); }
class X86X87SwitchRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86X87SwitchRT, Verify) { roundTripX86(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeSwitchTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Switch selects a binary op between two resident f80 values.
    {p+"_swacc",
     t+" "+p+"_swacc("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u; long double x=2.0L, y=3.0L, s=0;\n"
     "  for(int i=0;i<180;i++){ long double v=(long double)(int)(u%50)*0.1L;\n"
     "    switch(u%5u){\n"
     "      case 0: x=x*1.0000003L+v; break;\n"
     "      case 1: y=y-v*0.5L; break;\n"
     "      case 2: x=x+y*0.25L; break;\n"
     "      case 3: y=y*1.0000007L-v; break;\n"
     "      default: x=x*0.5L+y*0.5L; break; }\n"
     "    s=s+(x-y)*0.001L;\n"
     "    u=u*1103515245u+12345u; }\n"
     "  unsigned long long m; __builtin_memcpy(&m,&s,8);\n"
     "  return ("+t+")(unsigned)((unsigned)m^(unsigned)(m>>32)); }\n",
     {0x32ULL}, "X87Switch", 2},

    // Denser switch (0..9) with the accumulator and a constant both resident.
    {p+"_swdense",
     t+" "+p+"_swdense("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u; long double acc=0.5L, c=1000.0L; unsigned h=0;\n"
     "  for(int i=0;i<200;i++){\n"
     "    switch(u%10u){\n"
     "      case 0: acc=acc+1.0L; break;   case 1: acc=acc*1.1L; break;\n"
     "      case 2: acc=acc-0.5L; break;   case 3: acc=acc*0.9L; break;\n"
     "      case 4: acc=acc+c*0.001L; break; case 5: acc=acc*1.0000005L; break;\n"
     "      case 6: acc=acc-c*0.0005L; break; case 7: acc=acc+2.0L; break;\n"
     "      case 8: acc=acc*0.5L; break;   default: acc=acc+0.125L; break; }\n"
     "    if(acc>c) { acc=acc-c; h++; }\n"
     "    u=u*1103515245u+12345u; }\n"
     "  unsigned long long m; __builtin_memcpy(&m,&acc,8);\n"
     "  return ("+t+")(unsigned)(h^(unsigned)m^(unsigned)(m>>32)); }\n",
     {0x43ULL}, "X87Switch", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeSwitchTC("x64sw", "long");
static const std::vector<RoundTripTC> kX86 = makeSwitchTC("x86sw", "int");

INSTANTIATE_TEST_SUITE_P(X87Switch, X64X87SwitchRT,
                         ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(X87Switch, X86X87SwitchRT,
                         ::testing::ValuesIn(kX86), rtTCName);
