//===- AllPlatform_OptStress300RTTests.cpp - variadic ABI probe ==========//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// -O2 kernels stressing the variadic (stdarg) calling convention, which has its
// own lift surface not covered elsewhere: the register save-area prologue, the
// __va_list cursor and va_arg offset logic, the overflow (stack) area, and
// va_copy.  Each entry calls a `noinline` variadic helper with runtime-derived
// arguments so clang must emit the real vararg ABI:
//
//   * vsum   - 5 int varargs (register-passed save area + va_arg GP offset).
//   * vmany  - 12 int varargs (forces the overflow / stack area).
//   * vmix   - per-position ops over varargs (ordering-sensitive).
//   * vll    - long long varargs (8-byte va_arg; register/stack pairs on 32-bit).
//   * vcnt   - runtime count loop over a varying number of varargs.
//   * vcopy  - va_copy: traverse the same list twice with different weights.
//
// AArch64 keeps separate GP/FP save areas in __va_list (__gr_top/__gr_offs);
// i386 passes everything on the cdecl stack; ARM32 saves r0-r3 then overflows
// to the stack.  All varargs are integer/long long so every target stays
// libcall-free.  Integer in / integer out, folded to one return.  Four, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress300RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress300RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress300RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress300RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress300RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress300RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress300RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress300RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress300TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 5 int varargs: register save area + va_arg GP offset advance.
    {p+"_vsum",
     "int "+p+"_vsum(int n, ...) __attribute__((noinline));\n"
     +t+" "+p+"_vsum_e("+t+" x){ unsigned u=(unsigned)x;\n"
     "  return ("+t+")"+p+"_vsum(5,(int)u,(int)(u>>3),(int)(u*7u),\n"
     "    (int)(u^0x55u),(int)(u-13u)); }\n"
     "int "+p+"_vsum(int n, ...){ __builtin_va_list ap; __builtin_va_start(ap,n);\n"
     "  int s=0; for(int i=0;i<n;i++) s=s*31+__builtin_va_arg(ap,int);\n"
     "  __builtin_va_end(ap); return s; }\n",
     {0x12345u}, "OptStress300", 2},

    // 12 int varargs: forces the overflow (stack) area beyond the save area.
    {p+"_vmany",
     "int "+p+"_vmany(int n, ...) __attribute__((noinline));\n"
     +t+" "+p+"_vmany_e("+t+" x){ unsigned u=(unsigned)x;\n"
     "  return ("+t+")"+p+"_vmany(12,(int)u,(int)(u+1u),(int)(u*2u),(int)(u^3u),\n"
     "    (int)(u-4u),(int)(u*5u),(int)(u+6u),(int)(u^7u),(int)(u-8u),\n"
     "    (int)(u*9u),(int)(u+10u),(int)(u^11u)); }\n"
     "int "+p+"_vmany(int n, ...){ __builtin_va_list ap; __builtin_va_start(ap,n);\n"
     "  int s=0; for(int i=0;i<n;i++) s=s*131+__builtin_va_arg(ap,int);\n"
     "  __builtin_va_end(ap); return s; }\n",
     {0x23456u}, "OptStress300", 2},

    // Per-position ops over varargs (ordering-sensitive add/sub/xor).
    {p+"_vmix",
     "int "+p+"_vmix(int n, ...) __attribute__((noinline));\n"
     +t+" "+p+"_vmix_e("+t+" x){ unsigned u=(unsigned)x;\n"
     "  return ("+t+")"+p+"_vmix(6,(int)u,(int)(u>>2),(int)(u*3u),\n"
     "    (int)(u^0xAAu),(int)(u+99u),(int)(u-31u)); }\n"
     "int "+p+"_vmix(int n, ...){ __builtin_va_list ap; __builtin_va_start(ap,n);\n"
     "  int s=0; for(int i=0;i<n;i++){ int v=__builtin_va_arg(ap,int);\n"
     "    if(i&1) s-=v; else s+=v; s^=(v<<(i&7)); }\n"
     "  __builtin_va_end(ap); return s; }\n",
     {0x34567u}, "OptStress300", 2},

    // long long varargs: 8-byte va_arg, register/stack pairs on 32-bit.
    {p+"_vll",
     "long long "+p+"_vll(int n, ...) __attribute__((noinline));\n"
     +t+" "+p+"_vll_e("+t+" x){ long long a=(long long)(unsigned)x;\n"
     "  return ("+t+")"+p+"_vll(4, a, a*3+1, a^0x123456789LL, a-7); }\n"
     "long long "+p+"_vll(int n, ...){ __builtin_va_list ap; __builtin_va_start(ap,n);\n"
     "  long long s=0; for(int i=0;i<n;i++) s=s*131+__builtin_va_arg(ap,long long);\n"
     "  __builtin_va_end(ap); return s; }\n",
     {0x45678u}, "OptStress300", 2},

    // Runtime count loop over a varying number of varargs.
    {p+"_vcnt",
     "int "+p+"_vcnt(int n, ...) __attribute__((noinline));\n"
     +t+" "+p+"_vcnt_e("+t+" x){ unsigned u=(unsigned)x; int n=(int)(3u+(u&3u));\n"
     "  return ("+t+")"+p+"_vcnt(n,(int)u,(int)(u>>4),(int)(u*5u),\n"
     "    (int)(u^0x33u),(int)(u+7u),(int)(u-9u)); }\n"
     "int "+p+"_vcnt(int n, ...){ __builtin_va_list ap; __builtin_va_start(ap,n);\n"
     "  int s=1; for(int i=0;i<n;i++) s=s*7+__builtin_va_arg(ap,int);\n"
     "  __builtin_va_end(ap); return s; }\n",
     {0x56789u}, "OptStress300", 2},

    // va_copy: traverse the same list twice with different weights.
    {p+"_vcopy",
     "int "+p+"_vcopy(int n, ...) __attribute__((noinline));\n"
     +t+" "+p+"_vcopy_e("+t+" x){ unsigned u=(unsigned)x;\n"
     "  return ("+t+")"+p+"_vcopy(5,(int)u,(int)(u>>5),(int)(u*11u),\n"
     "    (int)(u^0x7Fu),(int)(u+42u)); }\n"
     "int "+p+"_vcopy(int n, ...){ __builtin_va_list a,b; __builtin_va_start(a,n);\n"
     "  __builtin_va_copy(b,a); int s1=0,s2=0;\n"
     "  for(int i=0;i<n;i++) s1+=__builtin_va_arg(a,int);\n"
     "  for(int i=0;i<n;i++) s2+=__builtin_va_arg(b,int)*(i+1);\n"
     "  __builtin_va_end(a); __builtin_va_end(b); return s1*3+s2; }\n",
     {0x6789Au}, "OptStress300", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress300TC("x64o300", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress300TC("x86o300", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress300TC("a64o300", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress300TC("armo300", "int");

INSTANTIATE_TEST_SUITE_P(OptStress300, X64OptStress300RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress300, X86OptStress300RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress300, A64OptStress300RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress300, ARM32OptStress300RT, ::testing::ValuesIn(kARM), rtTCName);
