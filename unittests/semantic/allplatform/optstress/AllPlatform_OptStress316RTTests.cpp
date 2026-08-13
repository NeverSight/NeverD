//===- AllPlatform_OptStress316RTTests.cpp - int arg == function VA -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A small INTEGER call argument whose value coincides with an ADDRESS-TAKEN
// function's entry VA must stay an integer — it must NOT be symbolized into that
// function's address.  This is the call-argument / general-constant arm of the
// #456/#459/#499/#518 "a small constant collides with a relocation / code target
// VA and getVar redirects it to a pointer" family.  #518/#520 closed the STORE-
// value (vtable-base) arm; this probe closes the function-pointer arm of getVar.
//
// Root cause (MedLLVMEmitter::getVar): a constant equal to a function entry whose
// address is recorded in CodeRefTargets was emitted as a relocatable
// `ptrtoint @func` so a genuine function pointer (`lea`/`adrp+add`/literal pool)
// relinks correctly.  But that arm — unlike its sibling global-data arm directly
// above it — carried NO width guard, so a plain `int` argument `f(0xA0)` whose
// value merely equals a callee sitting at VA 0xA0 (its address taken elsewhere to
// keep it live) was rewritten to `ptrtoint(@callee)`: the recompiled call passed
// the callee's *relinked* address instead of the literal 0xA0, corrupting the
// result.  A function pointer is materialized at EXACTLY pointer width, so the
// fix gates the arm on `V.Size >= PointerSize`: a sub-pointer-width constant
// cannot be a function pointer and stays an integer; the genuine pointer-width
// code-pointer target / stored function pointer is unaffected.
//
// Probe: a deterministic chain of DIRECT `int helper(int,int)` calls whose helper
// address is also taken (`asm("" : "+r"(fp))` keeps it live, so the entry VA lands
// in CodeRefTargets) passing a spread of small constant second arguments.  On
// x86-64 the 4-byte `int` argument 0xA0, and on AArch64 0x74, coincide with the
// helper's low entry VA; before the fix that constant became `ptrtoint(@helper)`
// (wrong), after it stays an integer.  i386/ARM32 are controls in this layout
// (their helper VA does not fall on a passed constant, so no collision is
// exercised there).  Direct (not indirect) calls and pure-int (no i64) math keep
// the probe focused on the constant-symbolization defect.  -O2, all four targets.
//
// KNOWN-OPEN (orthogonal, deliberately not exercised here — focused follow-ups):
//   * On a 32-bit target a function pointer is itself 4 bytes, so the width guard
//     cannot tell an `int` argument that equals a low code VA from a genuine
//     function pointer; distinguishing them needs PER-USE relocation / code-ref
//     tracking the lift does not yet carry.
//   * An INDIRECT i64-threaded variant of this probe additionally surfaced (a) a
//     first-indirect-call argument-recovery drop, (b) an x86-64 colliding-call
//     argument-width misrecovery, and (c) an ARM32 indirect i64-RETURN threading
//     high-half drop (the indirect form of the #311/#441 register-pair family) —
//     each left for a dedicated probe.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress316RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress316RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress316RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress316RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress316RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress316RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress316RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress316RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress316TC(const char *prefix,
                                                   const char *T, int Opt) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> v = {
    // Direct int->int call chain whose helper address is taken (kept live by the
    // asm barrier).  The constant second arguments span the low VA range so one
    // coincides with the helper's entry VA on the 64-bit targets (x86-64 0xA0,
    // AArch64 0x74); that argument must stay an integer, not become &helper.
    {p+"_fncoll",
     "static int "+p+"_h(int s,int k) __attribute__((noinline));\n"
     +t+" "+p+"_fncoll("+t+" a){\n"
     "  int (*fp)(int,int)="+p+"_h; __asm__(\"\" : \"+r\"(fp));\n"
     "  int acc=(int)a;\n"
     "  acc="+p+"_h(acc,0x44); acc="+p+"_h(acc,0x50); acc="+p+"_h(acc,0x5C);\n"
     "  acc="+p+"_h(acc,0x74); acc="+p+"_h(acc,0x7C); acc="+p+"_h(acc,0xA0);\n"
     "  acc="+p+"_h(acc,0xA8); acc="+p+"_h(acc,0xB0); acc="+p+"_h(acc,0xBC);\n"
     "  acc="+p+"_h(acc,0xC8); acc="+p+"_h(acc,0xDC); acc="+p+"_h(acc,0xEC);\n"
     "  return ("+t+")acc; }\n"
     "static int "+p+"_h(int s,int k){ return s*1103515245 + k*2654435761u + (s^k); }\n",
     {0x1234u}, "OptStress316", Opt},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress316TC("x64o316", "long", 2);
static const std::vector<RoundTripTC> kX86 = makeOptStress316TC("x86o316", "int", 2);
static const std::vector<RoundTripTC> kA64 = makeOptStress316TC("a64o316", "long", 2);
static const std::vector<RoundTripTC> kARM = makeOptStress316TC("armo316", "int", 2);

INSTANTIATE_TEST_SUITE_P(OptStress316, X64OptStress316RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress316, X86OptStress316RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress316, A64OptStress316RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress316, ARM32OptStress316RT, ::testing::ValuesIn(kARM), rtTCName);
