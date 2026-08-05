//===- X86_X87ConstLoadRTTests.cpp - x87 built-in constant loads -*- C++ -*-=//
//
// FLD1/FLDZ load 1.0/0.0 and were already modeled, but the transcendental
// constant loads FLDPI/FLDL2E/FLDL2T/FLDLG2/FLDLN2 were lifted as a bare
// placeholder intrinsic that pushes garbage onto the x87 stack (the LLVM
// backend folds an unhandled value-producing INTRINSIC to 0).  Each must push
// its correctly-rounded double bit pattern (x86reg::FPU_CONST_*), i.e. the
// 80->64 round-to-nearest result QEMU produces on `fstpl`.
//
// Each kernel loads one constant, stores it back as a double, then folds the
// full 64-bit pattern into the int return so a single wrong bit fails.  x87 is
// x86-family only, so we run x86-64 + i386.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64X87ConstRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64X87ConstRT, Verify) { roundTripX64(GetParam()); }

class X86X87ConstRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86X87ConstRT, Verify) { roundTripX86(GetParam()); }

// clang-format off
static std::string konst(const std::string &p, const std::string &name,
                         const std::string &insn) {
  return
    "int "+p+"_"+name+"(int a){ double d;\n"
    "  __asm__ volatile(\""+insn+"\\n\\tfstpl %0\":\"=m\"(d)::\"st\");\n"
    "  unsigned long long u; __builtin_memcpy(&u,&d,8);\n"
    "  return (int)(u ^ (u>>32)) + (a&0); }\n";
}

static std::vector<RoundTripTC> makeConst(const char *prefix) {
  std::string p = prefix;
  std::vector<RoundTripTC> v;
  struct { const char *name, *insn; } ks[] = {
    {"fld1",   "fld1"},   {"fldz",   "fldz"},   {"fldpi",  "fldpi"},
    {"fldl2e", "fldl2e"}, {"fldl2t", "fldl2t"}, {"fldlg2", "fldlg2"},
    {"fldln2", "fldln2"},
  };
  for (auto &k : ks)
    v.push_back({p+"_"+k.name, konst(p, k.name, k.insn), {1ULL}, "X87Const",
                 0, "-mno-sse -mfpmath=387"});
  return v;
}

static const std::vector<RoundTripTC> kX64Const = makeConst("x64");
static const std::vector<RoundTripTC> kX86Const = makeConst("x86");
// clang-format on

INSTANTIATE_TEST_SUITE_P(X87Const, X64X87ConstRT, ::testing::ValuesIn(kX64Const),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(X87Const, X86X87ConstRT, ::testing::ValuesIn(kX86Const),
                         rtTCName);
