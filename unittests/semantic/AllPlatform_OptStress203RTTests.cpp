//===- AllPlatform_OptStress203RTTests.cpp - by-value struct ABI ========//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Calling-ABI roundtrip probes for by-value aggregate arguments / returns and
// mutable aggregate homes -- the surface the #491-494 mutable-stack-param work
// touched, extended from scalar overflow args to whole structs.  Each entry has
// a plain T(T) ABI and calls a noinline helper so clang keeps a real call that
// passes / returns the aggregate through the platform ABI (x86-64 MEMORY class
// on the stack, AArch64 by-reference for >16B, i386/ARM32 stack copies).
//
//   * bigstruct - a 6-field struct passed by value, fields summed in the callee.
//   * structmut - a by-value struct copy mutated in a loop inside the callee
//                 (the callee's aggregate home must be mutable, like #493).
//   * wideovfmut- 10 long long args (overflowing the int param regs on every
//                 target) with the stack-passed ones mutated in a loop.
//   * structret - a large struct returned by value (sret hidden pointer).
//   * mixstruct - a struct followed by trailing scalar args (mixed reg/stack).
//   * structptr - the address of a local struct escapes to a noinline writer
//                 that RMWs it; the caller must observe the writes.
//
// Helpers use only add/sub/mul/xor/shift (no float, no 64-bit divide on 32-bit,
// structs <= 48B so clang inlines the copies -- no memcpy libcall).  -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress203RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress203RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress203RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress203RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress203RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress203RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress203RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress203RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress203TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // A 6-field struct passed by value, fields summed in the callee.
    {p+"_bigstruct",
     "typedef struct{"+t+" a,b,c,d,e,f;}"+p+"_S6;\n"
     +t+" "+p+"_sum6("+p+"_S6 s) __attribute__((noinline));\n"
     +t+" "+p+"_bigstruct("+t+" x){ "+p+"_S6 s;\n"
     "  s.a=x; s.b=x+1; s.c=x*2; s.d=x-3; s.e=x*5; s.f=x+7;\n"
     "  return "+p+"_sum6(s); }\n"
     +t+" "+p+"_sum6("+p+"_S6 s){\n"
     "  return s.a+2*s.b+3*s.c+4*s.d+5*s.e+6*s.f; }\n",
     {0x1234ULL}, "OptStress203", 2},

    // A by-value struct copy mutated in a loop inside the callee.
    {p+"_structmut",
     "typedef struct{"+t+" a,b,c,d;}"+p+"_S4;\n"
     +t+" "+p+"_mut4("+p+"_S4 s,int n) __attribute__((noinline));\n"
     +t+" "+p+"_structmut("+t+" x){ "+p+"_S4 s;\n"
     "  s.a=x; s.b=x+1; s.c=x+2; s.d=x+3;\n"
     "  return "+p+"_mut4(s,10); }\n"
     +t+" "+p+"_mut4("+p+"_S4 s,int n){\n"
     "  for(int i=0;i<n;i++){ s.a=s.a*3+s.b; s.b^=s.a; s.c+=s.a-s.b; s.d=(s.d<<1)^s.c; }\n"
     "  return s.a^s.b^s.c^s.d; }\n",
     {0x2222ULL}, "OptStress203", 2},

    // 10 long long args (overflow on every target); stack ones mutated in a loop.
    {p+"_wideovfmut",
     "long long "+p+"_w10(long long,long long,long long,long long,long long,\n"
     "  long long,long long,long long,long long,long long) __attribute__((noinline));\n"
     +t+" "+p+"_wideovfmut("+t+" x){ long long a=(long long)x;\n"
     "  return ("+t+")"+p+"_w10(a,a+1,a+2,a+3,a+4,a+5,a+6,a+7,a+8,a+9); }\n"
     "long long "+p+"_w10(long long a,long long b,long long c,long long d,long long e,\n"
     "  long long f,long long g,long long h,long long i,long long j){\n"
     "  for(int k=0;k<8;k++){ j=j*3+i; i^=j; h+=i-j; g=(g<<1)^h; }\n"
     "  return a+b+c+d+e+f+g+h+i+j; }\n",
     {0x40ULL}, "OptStress203", 2},

    // A large struct returned by value (sret hidden pointer).
    {p+"_structret",
     "typedef struct{"+t+" a,b,c,d,e;}"+p+"_R5;\n"
     +p+"_R5 "+p+"_mk5("+t+" x) __attribute__((noinline));\n"
     +t+" "+p+"_structret("+t+" x){ "+p+"_R5 r="+p+"_mk5(x);\n"
     "  return r.a^r.b^r.c^r.d^r.e; }\n"
     +p+"_R5 "+p+"_mk5("+t+" x){ "+p+"_R5 r;\n"
     "  r.a=x; r.b=x*3; r.c=x-7; r.d=x*11; r.e=x+13; return r; }\n",
     {0x55ULL}, "OptStress203", 2},

    // A struct followed by trailing scalar args (mixed reg/stack lanes).
    {p+"_mixstruct",
     "typedef struct{"+t+" a,b,c;}"+p+"_S3;\n"
     +t+" "+p+"_mix3("+p+"_S3 s,"+t+" p,"+t+" q,"+t+" r) __attribute__((noinline));\n"
     +t+" "+p+"_mixstruct("+t+" x){ "+p+"_S3 s;\n"
     "  s.a=x; s.b=x+1; s.c=x+2;\n"
     "  return "+p+"_mix3(s,x*2,x*3,x*4); }\n"
     +t+" "+p+"_mix3("+p+"_S3 s,"+t+" p,"+t+" q,"+t+" r){\n"
     "  return s.a+s.b+s.c+p*q-r; }\n",
     {0x99ULL}, "OptStress203", 2},

    // Address of a local struct escapes to a noinline writer that RMWs it.
    {p+"_structptr",
     "typedef struct{"+t+" a,b,c,d;}"+p+"_S4b;\n"
     "void "+p+"_upd4("+p+"_S4b *s,int n) __attribute__((noinline));\n"
     +t+" "+p+"_structptr("+t+" x){ "+p+"_S4b s;\n"
     "  s.a=x; s.b=x+1; s.c=x+2; s.d=x+3;\n"
     "  "+p+"_upd4(&s,12);\n"
     "  return s.a^s.b^s.c^s.d; }\n"
     "void "+p+"_upd4("+p+"_S4b *s,int n){\n"
     "  for(int i=0;i<n;i++){ s->a=s->a*3+s->b; s->b^=s->a; s->c+=s->a; s->d-=s->b; } }\n",
     {0x77ULL}, "OptStress203", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress203TC("x64o203", "long");
static const std::vector<RoundTripTC> kA64 = makeOptStress203TC("a64o203", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress203TC("armo203", "int");
static const std::vector<RoundTripTC> kX86 = makeOptStress203TC("x86o203", "int");

INSTANTIATE_TEST_SUITE_P(OptStress203, X64OptStress203RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress203, X86OptStress203RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress203, A64OptStress203RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress203, ARM32OptStress203RT, ::testing::ValuesIn(kARM), rtTCName);
