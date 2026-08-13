//===- AllPlatform_VariadicAbiRTTests.cpp - variadic call ABI --*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Variadic (`...`) calls take an ABI path none of the earlier call probes reach:
// the callee materializes a register save area in its prologue (x86-64 spills the
// integer arg registers plus, gated on AL, the XMM registers; AArch64 spills the
// GPR/FPR save areas; arm32 spills r0-r3) and `va_arg` walks gp_offset / the
// overflow area.  The caller side must also satisfy the variadic convention
// (x86-64 sets AL = number of vector registers used).  A lifter that models the
// callee's fixed prologue spills as ordinary stack params, or drops AL on the
// caller, mis-sums every argument.
//
//   * vsum_i   - sum of N int varargs (register + overflow area walk).
//   * vsum_big - more varargs than parameter registers (forces the overflow
//                area / pure stack on i386).
//   * vmax_i   - running max over int varargs (varargs feeding a compare/select).
//   * vsum_ll  - mixed fixed int + variadic long long (8-byte slot stepping).
//
// Every kernel folds to a single integer return so native and lifted emulation
// compare a scalar.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VariadicAbiRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VariadicAbiRT, Verify) { roundTripX64(GetParam()); }
class X86VariadicAbiRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86VariadicAbiRT, Verify) { roundTripX86(GetParam()); }
class A64VariadicAbiRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VariadicAbiRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32VariadicAbiRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VariadicAbiRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeVariadicAbiTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Sum of four int varargs: register save area + overflow walk.
    {p+"_vsum_i",
     "#include <stdarg.h>\n"
     "static int "+p+"_si(int n, ...) __attribute__((noinline));\n"
     +t+" "+p+"_vsum_i("+t+" a){\n"
     "  unsigned u=(unsigned)a;\n"
     "  return ("+t+")"+p+"_si(4,(int)u,(int)(u*3u+1u),(int)(u^0x55u),(int)(u+7u)); }\n"
     "static int "+p+"_si(int n, ...){\n"
     "  va_list ap; va_start(ap,n); int s=0;\n"
     "  for(int i=0;i<n;i++) s=s*31+va_arg(ap,int);\n"
     "  va_end(ap); return s; }\n",
     {0x41ULL}, "VariadicAbi", 2},

    // Eight int varargs: forces the overflow area / pure stack on i386.
    {p+"_vsum_big",
     "#include <stdarg.h>\n"
     "static int "+p+"_sb(int n, ...) __attribute__((noinline));\n"
     +t+" "+p+"_vsum_big("+t+" a){\n"
     "  unsigned u=(unsigned)a;\n"
     "  return ("+t+")"+p+"_sb(8,(int)u,(int)(u+1u),(int)(u+2u),(int)(u+3u),\n"
     "    (int)(u+4u),(int)(u+5u),(int)(u+6u),(int)(u+7u)); }\n"
     "static int "+p+"_sb(int n, ...){\n"
     "  va_list ap; va_start(ap,n); int s=0;\n"
     "  for(int i=0;i<n;i++) s=s*131+va_arg(ap,int);\n"
     "  va_end(ap); return s; }\n",
     {0x9bULL}, "VariadicAbi", 2},

    // Running max over int varargs (varargs feeding a compare / select).
    {p+"_vmax_i",
     "#include <stdarg.h>\n"
     "static int "+p+"_mx(int n, ...) __attribute__((noinline));\n"
     +t+" "+p+"_vmax_i("+t+" a){\n"
     "  unsigned u=(unsigned)a;\n"
     "  return ("+t+")"+p+"_mx(5,(int)u,(int)(u^0x7fu),(int)(0-u),(int)(u*5u),(int)(u>>1)); }\n"
     "static int "+p+"_mx(int n, ...){\n"
     "  va_list ap; va_start(ap,n); int m=-2147483647-1;\n"
     "  for(int i=0;i<n;i++){ int v=va_arg(ap,int); if(v>m) m=v; }\n"
     "  va_end(ap); return m; }\n",
     {0xa7ULL}, "VariadicAbi", 2},

    // Mixed fixed int + variadic long long (8-byte slot stepping).
    {p+"_vsum_ll",
     "#include <stdarg.h>\n"
     "static long long "+p+"_sl(int n, ...) __attribute__((noinline));\n"
     +t+" "+p+"_vsum_ll("+t+" a){\n"
     "  unsigned u=(unsigned)a;\n"
     "  long long r="+p+"_sl(3,(long long)u<<10,(long long)(u+1u)<<20,(long long)(u+2u)<<3);\n"
     "  return ("+t+")(unsigned)(r ^ (r>>32)); }\n"
     "static long long "+p+"_sl(int n, ...){\n"
     "  va_list ap; va_start(ap,n); long long s=0;\n"
     "  for(int i=0;i<n;i++) s=s*1000003+va_arg(ap,long long);\n"
     "  va_end(ap); return s; }\n",
     {0x6dULL}, "VariadicAbi", 2},
  };
}
// clang-format on

// Every shape round-trips on all four targets: the register save area forwards
// through ordinary register parameters, and the overflow (incoming-stack)
// arguments are recovered as trailing stack parameters and spilled into the
// frame headroom so the va_arg walk reads them — see the Unicorn unsupported-instructions doc
// #429 (x64/a64 overflow area, ARM32 vectorized walk + long-long varargs) and
// #428 (i386 clang IPO + PIC noise).
static const std::vector<RoundTripTC> kX64 = makeVariadicAbiTC("x64va", "long");
static const std::vector<RoundTripTC> kX86 = makeVariadicAbiTC("x86va", "int");
static const std::vector<RoundTripTC> kA64 = makeVariadicAbiTC("a64va", "long");
static const std::vector<RoundTripTC> kARM = makeVariadicAbiTC("armva", "int");

INSTANTIATE_TEST_SUITE_P(VariadicAbi, X64VariadicAbiRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(VariadicAbi, X86VariadicAbiRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(VariadicAbi, A64VariadicAbiRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(VariadicAbi, ARM32VariadicAbiRT, ::testing::ValuesIn(kARM), rtTCName);
