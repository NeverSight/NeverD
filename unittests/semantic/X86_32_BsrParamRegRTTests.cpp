//===- X86_32_BsrParamRegRTTests.cpp - i386 regparm bsr-preserve param ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Guards the #500 i386 register-parameter scratch heuristic
// (`liveInOnlyFeedsScratch` / `isBsrBsfPreserve`) for the regparm-callee variant
// (the OptStress211-215 probes cover the cdecl entry function).  That fix drops
// a "live-in" register parameter whose only use is the BSR/BSF zero-source
// preserve `SELECT(src==0, old_dst, (bits-1)-clz)`.  A GENUINE i386 regparm
// argument used ONLY as that preserve value has the same MedIR shape, so the
// heuristic must keep it: a static regparm callee whose second argument feeds
// only the bsr/bsf preserve must still round-trip, returning the argument when
// the scan source is zero.  -O2 (the optimizer-on baseline the recovery targets).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X86BsrParamRegRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86BsrParamRegRT, Verify) { roundTripX86(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX86 = {
  // Isolation: single cdecl function, no regparm, no helper.  `31-clz(x)` lowers
  // to LZCNT in the recompiled image; if Unicorn's i386 CPU lacks ABM the f3
  // prefix is ignored and the opcode runs as BSR, silently inverting the result.
  {"iso_clz",
   "int f(int x){ return x ? (31-__builtin_clz(x)) : 99; }\n",
   {0x40u}, "BsrParamReg", 2},
  {"iso_clz_zero",
   "int f(int x){ return x ? (31-__builtin_clz(x)) : 99; }\n",
   {0u}, "BsrParamReg", 2},
  {"iso_ctz",
   "int f(int x){ return x ? __builtin_ctz(x) : 77; }\n",
   {0x40u}, "BsrParamReg", 2},

  // regparm(2): helper(x->EAX, preserve->EDX).  preserve is only the bsr old_dst.
  // Zero source every 4th call -> result == preserve; a dropped EDX param diverges.
  {"bsr_edx_preserve",
   "static int bpr_h(int x,int preserve) __attribute__((regparm(2),noinline));\n"
   "int f(int a){ unsigned acc=(unsigned)a|1u;\n"
   "  for(int i=0;i<50;i++){ int x=((i&3)==0)?0:(int)(acc^(unsigned)i);\n"
   "    acc=(unsigned)bpr_h(x,(int)acc)*131u+(unsigned)i; }\n"
   "  return (int)acc; }\n"
   "static int bpr_h(int x,int preserve){ unsigned r=(unsigned)preserve;\n"
   "  __asm__(\"bsrl %1,%0\":\"+r\"(r):\"r\"((unsigned)x):\"cc\");\n"
   "  return (int)r; }\n",
   {0x12345u}, "BsrParamReg", 2},

  // bsf variant: preserve in EDX is the trailing-scan old_dst.
  {"bsf_edx_preserve",
   "static int bpf_h(int x,int preserve) __attribute__((regparm(2),noinline));\n"
   "int f(int a){ unsigned acc=(unsigned)a|1u;\n"
   "  for(int i=0;i<50;i++){ int x=((i&3)==0)?0:(int)(acc^(unsigned)i);\n"
   "    acc=(unsigned)bpf_h(x,(int)acc)*131u+(unsigned)i; }\n"
   "  return (int)acc; }\n"
   "static int bpf_h(int x,int preserve){ unsigned r=(unsigned)preserve;\n"
   "  __asm__(\"bsfl %1,%0\":\"+r\"(r):\"r\"((unsigned)x):\"cc\");\n"
   "  return (int)r; }\n",
   {0x6789Au}, "BsrParamReg", 2},

  // C-level `x ? 31-clz(x) : preserve`: the preserved arg reaches the select via
  // a path that must remain a genuine parameter (negative-control shape).
  {"clz_ternary_preserve",
   "static unsigned cpr_h(unsigned x,unsigned preserve) __attribute__((regparm(2),noinline));\n"
   "int f(int a){ unsigned acc=(unsigned)a|1u;\n"
   "  for(int i=0;i<50;i++){ unsigned x=((i&3)==0)?0u:(acc^(unsigned)i);\n"
   "    acc=cpr_h(x,acc)*131u+(unsigned)i; }\n"
   "  return (int)acc; }\n"
   "static unsigned cpr_h(unsigned x,unsigned preserve){\n"
   "  return x ? (unsigned)(31-__builtin_clz(x)) : preserve; }\n",
   {0xBCDEFu}, "BsrParamReg", 2},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(BsrParamReg, X86BsrParamRegRT,
                         ::testing::ValuesIn(kX86), rtTCName);
