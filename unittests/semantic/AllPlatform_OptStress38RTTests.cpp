//===- AllPlatform_OptStress38RTTests.cpp - jump-table relay probes -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A control-flow probe family targeting the two deep jump-table-recovery shapes
// #448 surfaced and deferred: an indirect switch dispatch whose computed target
// is staged through a *frame slot* before the indirect branch, and a many-way
// state-machine switch table.  Both are the spill/relay forms that the
// single-instruction relative-table heuristic and the cross-instruction
// resolver have to cooperate on.  Each kernel folds to a single integer:
//
//   * duffreg   - an 8-way Duff's-device unrolled switch threaded through a loop
//                 with enough live accumulators that i386 -fPIC spills the
//                 computed GOTOFF jump target to a stack slot and dispatches via
//                 `jmp *(%esp)` (the #448 stack-slot relay).
//   * statemach - a 16-state machine switch table; the next state is produced by
//                 the current arm, so the dispatch edge sequence is fully
//                 dynamic (ARM32 lowers it to an inline PC-relative word table).
//   * fallspill - a fallthrough switch in a loop with extra live state so the
//                 dispatch target is again staged through the stack.
//   * nestsw    - nested switches whose inner index is produced by the outer
//                 arm, stressing multiple tables in one function.
//
// Integer-only, single integer return, bounded, no 64-bit divide, no library
// calls; all four targets at -O2, native vs lifted.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress38RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress38RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress38RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress38RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress38RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress38RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress38RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress38RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress38TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 8-way Duff's device with several live accumulators: under i386 -fPIC the
    // register pressure forces clang to spill the computed GOTOFF jump target to
    // a stack slot and dispatch via `jmp *(%esp)`.
    {p+"_duffreg",
     t+" "+p+"_duffreg("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u, y=(unsigned)a^0x55u, z=(unsigned)a*3u, w=~(unsigned)a, h=0;\n"
     "  int n=(int)(((unsigned)a&63u)+9u); int k=(n+7)/8;\n"
     "  switch(n&7){\n"
     "  case 0: do{ x=x*1103515245u+12345u; y^=x; h=h*131u+x+y;\n"
     "  case 7:    x=x*1103515245u+12345u; z+=x; h=h*131u+x+z;\n"
     "  case 6:    x=x*1103515245u+12345u; w-=x; h=h*131u+x+w;\n"
     "  case 5:    x=x*1103515245u+12345u; y+=z; h=h*131u+x+y;\n"
     "  case 4:    x=x*1103515245u+12345u; z^=w; h=h*131u+x+z;\n"
     "  case 3:    x=x*1103515245u+12345u; w+=y; h=h*131u+x+w;\n"
     "  case 2:    x=x*1103515245u+12345u; y^=w; h=h*131u+x+y;\n"
     "  case 1:    x=x*1103515245u+12345u; z-=y; h=h*131u+x+z;\n"
     "          }while(--k>0); }\n"
     "  return ("+t+")(unsigned)(h+x+y+z+w); }\n",
     {0x6dULL}, "OptStress38", 2},

    // 16-state machine switch table: next state derived from current arm.
    {p+"_statemach",
     t+" "+p+"_statemach("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0; unsigned st=(unsigned)a&15u;\n"
     "  for(int i=0;i<200;i++){\n"
     "    s=s*1103515245u+12345u;\n"
     "    switch(st){\n"
     "      case 0:  h+=s;            st=(s>>1)&15u; break;\n"
     "      case 1:  h^=s<<1;         st=(s>>2)&15u; break;\n"
     "      case 2:  h-=s>>3;         st=(s>>3)&15u; break;\n"
     "      case 3:  h+=s*3u;         st=(s>>4)&15u; break;\n"
     "      case 4:  h^=~s;           st=(s>>5)&15u; break;\n"
     "      case 5:  h=h*131u+s;      st=(s>>6)&15u; break;\n"
     "      case 6:  h+=(s&0xffu);    st=(s>>7)&15u; break;\n"
     "      case 7:  h^=(s>>8);       st=(s>>8)&15u; break;\n"
     "      case 8:  h-=s*7u;         st=(s>>9)&15u; break;\n"
     "      case 9:  h+=(s^0x5a5au);  st=(s>>10)&15u; break;\n"
     "      case 10: h^=(s<<3);       st=(s>>11)&15u; break;\n"
     "      case 11: h+=(s>>2)+1u;    st=(s>>12)&15u; break;\n"
     "      case 12: h=h*3u+s;        st=(s>>13)&15u; break;\n"
     "      case 13: h^=(s&0xf0f0u);  st=(s>>14)&15u; break;\n"
     "      case 14: h+=(s<<2)^s;     st=(s>>15)&15u; break;\n"
     "      default: h^=(s*9u);       st=(s>>16)&15u; break;\n"
     "    }\n"
     "    h+=(unsigned)i; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x5ULL}, "OptStress38", 2},

    // Fallthrough switch in a loop with extra carried state forcing a spill.
    {p+"_fallspill",
     t+" "+p+"_fallspill("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, p0=(unsigned)a, p1=~(unsigned)a, p2=(unsigned)a*7u, h=0;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u; unsigned w=s^p0;\n"
     "    switch((s>>5)&7u){\n"
     "      case 0: w+=p1;\n"
     "      case 1: w^=p2;\n"
     "      case 2: w=w*3u+1u; p0+=w; break;\n"
     "      case 3: w-=p0;\n"
     "      case 4: w=(w<<1)|(w>>31); p1^=w; break;\n"
     "      case 5: w|=0x55u;\n"
     "      case 6: w&=p2|1u;\n"
     "      default: w=~w; p2+=w; break; }\n"
     "    h=h*131u+w+p0+p1+p2; }\n"
     "  return ("+t+")(unsigned)(h^p0^p1^p2); }\n",
     {0xb2ULL}, "OptStress38", 2},

    // Nested switches: the inner dispatch index is produced by the outer arm.
    {p+"_nestsw",
     t+" "+p+"_nestsw("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<96;i++){ s=s*1103515245u+12345u; unsigned r=s;\n"
     "    switch((s>>3)&3u){\n"
     "      case 0: r^=0x11u; break;\n"
     "      case 1: r+=0x22u; break;\n"
     "      case 2: r=r*5u;   break;\n"
     "      default: r-=0x33u; break; }\n"
     "    switch((r>>7)&7u){\n"
     "      case 0: r+=1u; break;\n"
     "      case 1: r^=2u; break;\n"
     "      case 2: r-=3u; break;\n"
     "      case 3: r=(r<<2)|(r>>30); break;\n"
     "      case 4: r&=0xff00u; break;\n"
     "      case 5: r|=0x0fu; break;\n"
     "      case 6: r=~r; break;\n"
     "      default: r*=3u; break; }\n"
     "    h=h*131u+r; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x2fULL}, "OptStress38", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress38TC("x64o38", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress38TC("x86o38", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress38TC("a64o38", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress38TC("armo38", "int");

INSTANTIATE_TEST_SUITE_P(OptStress38, X64OptStress38RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress38, X86OptStress38RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress38, A64OptStress38RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress38, ARM32OptStress38RT, ::testing::ValuesIn(kARM), rtTCName);
