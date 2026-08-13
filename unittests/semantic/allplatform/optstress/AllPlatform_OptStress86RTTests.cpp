//===- AllPlatform_OptStress86RTTests.cpp - variadic global ptr args -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A global element address passed as a VARIADIC argument: `vfn(n, &G[i], ...)`.
// The variadic call's args are recovered as fixed integer params, so the callee
// declares the pointer varargs as `i64` rather than `ptr` — neither the
// declared-pointer-param path (#473) nor the LLVM-vararg path fires, and the
// computed `&G[i]` would be passed as a stale absolute VA the callee's va_arg
// dereferences.  The fix symbolizes a pointer-width integer call arg resolving
// to a global address even when the callee param is an integer.
//
//   * vargp   - three global element addresses as varargs, callee sums *p, p[1].
//   * vargpm  - varargs mix an int count and global pointers.
//   * vargp2  - two distinct globals reached through varargs in one call.
//
// SysV/AAPCS64 pass these varargs in registers (RSI/RDX/RCX, X1..X7); i386 cdecl
// passes them on the stack and ARM32 AAPCS in r1..r3 + stack.  All four targets
// now recover and symbolize them: the i386 leading-gap (clang drops the constant
// count arg, leaving slot 0 empty) is filled instead of truncating the arg list
// (#484), and the call-arg symbolization fires for any pointer-width integer arg
// reaching a recovered integer param.
//
// All integer, arrays seeded from the LCG, fold to one integer return.  -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress86RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress86RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress86RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress86RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress86RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress86RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress86RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress86RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress86TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Three global element addresses passed as varargs; callee derefs each.
    {p+"_vargp",
     "static int "+p+"_VG[8]={3,1,4,1,5,9,2,6};\n"
     "static long "+p+"_vs(int n,...) __attribute__((noinline));\n"
     +t+" "+p+"_vargp("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; long acc=0;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u;\n"
     "    acc+="+p+"_vs(3,&"+p+"_VG[(s>>5)&3u],&"+p+"_VG[(s>>9)&3u],\n"
     "                  &"+p+"_VG[(s>>13)&3u]);\n"
     "    acc^=acc>>6; }\n"
     "  return ("+t+")(acc+"+p+"_VG[0]); }\n"
     "static long "+p+"_vs(int n,...){\n"
     "  __builtin_va_list ap; __builtin_va_start(ap,n); long r=0;\n"
     "  for(int i=0;i<n;i++){ int*q=__builtin_va_arg(ap,int*); r+=q[0]+q[1]; }\n"
     "  __builtin_va_end(ap); return r; }\n",
     {0x51u}, "OptStress86", 2},

    // Varargs mix an int count and global pointers.
    {p+"_vargpm",
     "static int "+p+"_MG[8]={7,2,9,4,1,8,3,6};\n"
     "static long "+p+"_vm(int n,...) __attribute__((noinline));\n"
     +t+" "+p+"_vargpm("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; long acc=0;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u;\n"
     "    acc+="+p+"_vm(2,(int)((s>>3)&7u),&"+p+"_MG[(s>>6)&3u],\n"
     "                  &"+p+"_MG[(s>>9)&3u]);\n"
     "    acc^=acc>>5; }\n"
     "  return ("+t+")(acc+"+p+"_MG[2]); }\n"
     "static long "+p+"_vm(int n,...){\n"
     "  __builtin_va_list ap; __builtin_va_start(ap,n);\n"
     "  int k=__builtin_va_arg(ap,int); long r=k;\n"
     "  for(int i=0;i<n;i++){ int*q=__builtin_va_arg(ap,int*); r+=q[k&1]; }\n"
     "  __builtin_va_end(ap); return r; }\n",
     {0x52u}, "OptStress86", 2},

    // Two distinct globals reached through varargs in a single call.
    {p+"_vargp2",
     "static int "+p+"_PA[6]={5,3,5,8,9,7};\n"
     "static int "+p+"_PB[6]={9,3,2,3,8,4};\n"
     "static long "+p+"_v2(int n,...) __attribute__((noinline));\n"
     +t+" "+p+"_vargp2("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; long acc=0;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u;\n"
     "    acc+="+p+"_v2(2,&"+p+"_PA[(s>>4)&5u],&"+p+"_PB[(s>>8)&5u]);\n"
     "    acc^=acc>>6; }\n"
     "  return ("+t+")(acc+"+p+"_PA[0]+"+p+"_PB[5]); }\n"
     "static long "+p+"_v2(int n,...){\n"
     "  __builtin_va_list ap; __builtin_va_start(ap,n); long r=0;\n"
     "  for(int i=0;i<n;i++){ int*q=__builtin_va_arg(ap,int*); r+=q[0]; }\n"
     "  __builtin_va_end(ap); return r; }\n",
     {0x53u}, "OptStress86", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress86TC("x64o86", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress86TC("x86o86", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress86TC("a64o86", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress86TC("armo86", "int");

INSTANTIATE_TEST_SUITE_P(OptStress86, X64OptStress86RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress86, X86OptStress86RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress86, A64OptStress86RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress86, ARM32OptStress86RT, ::testing::ValuesIn(kARM), rtTCName);
