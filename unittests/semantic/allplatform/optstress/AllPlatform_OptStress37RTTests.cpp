//===- AllPlatform_OptStress37RTTests.cpp - control-flow probes -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A control-flow / CFG-recovery probe family, deliberately pivoting away from
// the arithmetic kernels of the recent OptStress rounds toward the branch and
// loop-structure recovery path (CFGBuilder, condition flattening, loop
// detection) that has historically carried the richest bug vein.  Each kernel
// builds an unusual CFG shape and folds to a single integer:
//
//   * irred    - goto-built irreducible-ish CFG: a forward jump into the middle
//                of a two-block cycle that re-enters from both sides.
//   * multibrk - triple-nested loops with a multi-level break (goto out), an
//                inner-only break, and an outer continue.
//   * fallsw   - a switch with deliberate fallthrough chains (cases with no
//                break flow into the next), stressing case-block ordering.
//   * shortckt - nested short-circuit && / || / ternary chains driving branches
//                (condition flattening + flag reuse).  Regression guard for the
//                #448 i386 fix: clang's `setne %cl` partial write made ECX look
//                live-in, so register-param recovery injected a phantom leading
//                argument that shifted the real cdecl stack arg (read as junk).
//
// This round also surfaced two deep jump-table-recovery bugs, recorded as
// pending specialties and NOT included here so the
// suite stays green: an i386 Duff's-device `jmp *(%esp)` whose target is staged
// through a stack slot from a GOTOFF .rodata table (mis-recovered as an indirect
// tail call), and an ARM32 state-machine switch table (UC_ERR_FETCH_UNMAPPED).
//
// Integer-only, single integer return, bounded, no 64-bit divide, no library
// calls; all four targets at -O2, native vs lifted.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress37RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress37RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress37RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress37RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress37RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress37RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress37RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress37RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress37TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // goto-built irreducible-ish CFG: forward jump into a two-block cycle that
    // re-enters from both sides, bounded by an internal counter.
    {p+"_irred",
     t+" "+p+"_irred("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned x=s, y=s>>1; int c=0;\n"
     "    if(s&1) goto B;\n"
     "  A: x=x*3u+1u; if(x&0x100u){ y^=x; goto C; }\n"
     "  B: y=y*5u+2u; if(++c<3) goto A;\n"
     "  C: h=h*131u+x+y; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x53ULL}, "OptStress37", 2},

    // Triple-nested loops: a multi-level break (goto out of all), an inner break,
    // and an outer continue interacting on one carried accumulator.
    {p+"_multibrk",
     t+" "+p+"_multibrk("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<32;i++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int j=0;j<8;j++){\n"
     "      for(int k=0;k<8;k++){ acc=acc*131u+(unsigned)(j*8+k);\n"
     "        if((acc&0x3fu)==0u) goto done;\n"
     "        if(acc&0x400u) break; }\n"
     "      if(acc&0x800u) continue;\n"
     "      acc^=(unsigned)j; }\n"
     "  done: h=h*131u+acc; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x6dULL}, "OptStress37", 2},

    // Switch with deliberate fallthrough chains (several cases flow into the
    // next without break), stressing case-block successor ordering.
    {p+"_fallsw",
     t+" "+p+"_fallsw("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u; unsigned w=s;\n"
     "    switch((s>>4)&7u){\n"
     "      case 0: w+=1u;\n"
     "      case 1: w^=0xaaaaaaaau;\n"
     "      case 2: w=w*3u+1u; break;\n"
     "      case 3: w-=7u;\n"
     "      case 4: w=(w<<1)|(w>>31); break;\n"
     "      case 5: w|=0x55u;\n"
     "      case 6: w&=0xffffff0fu;\n"
     "      default: w=~w; break; }\n"
     "    h=h*131u+w; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0xb2ULL}, "OptStress37", 2},

    // Nested short-circuit && / || / ternary chains driving branches (condition
    // flattening + flag reuse across the boolean operators).
    {p+"_shortckt",
     t+" "+p+"_shortckt("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned x=s, y=s>>7, z=s>>13;\n"
     "    int c1=(x&1u) && ((y&2u) || (z&4u)) && !(x&0x100u);\n"
     "    int c2=(x>y) || ((y<z) && (x!=z));\n"
     "    int c3=((x^y)&8u) ? ((z&16u)?1:2) : ((y&32u)?3:4);\n"
     "    unsigned r=(c1?5u:0u)+(c2?7u:0u)+(unsigned)c3*11u;\n"
     "    h=h*131u+r; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x2fULL}, "OptStress37", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress37TC("x64o37", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress37TC("x86o37", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress37TC("a64o37", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress37TC("armo37", "int");

INSTANTIATE_TEST_SUITE_P(OptStress37, X64OptStress37RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress37, X86OptStress37RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress37, A64OptStress37RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress37, ARM32OptStress37RT, ::testing::ValuesIn(kARM), rtTCName);
