//===- AllPlatform_OptStress26RTTests.cpp - opt-stress probes --*-C++*-=//
//
// PtrMem's `pun` kernel reinterprets a 32-bit value as half-words/bytes.  This
// round drives the 64-bit counterpart: a 64-bit storage cell accessed through
// its 32-bit and 8-bit sub-lanes.  On i386/ARM32 a 64-bit value lives in a
// register pair / two stack slots, so sub-lane punning is exactly sub-register
// aliasing one level wider, and a bitfield that straddles the 32-bit word
// boundary forces clang to splice bits across the two words -- both fragile in
// the lifter's load/store-width and partial-write modelling.
//
//   * pun64    - one u64 union read/written as two dwords and individual bytes.
//   * pun64idx - an array of u64 unions, runtime-indexed dword/byte sub-lanes.
//   * mixacc64 - a u64 whose low and high dwords evolve under different ops.
//   * punflip  - swap the two dwords of a u64 and byte-reverse, read back wide.
//   * structq  - a {lo,hi} dword struct used as a manual 64-bit add accumulator.
//   * bf64     - a bitfield struct whose fields straddle the 32-bit boundary.
//
// Every kernel is integer-only, folds to a single integer return and uses no
// 64-bit divide (the only 64-bit op that needs a runtime helper on 32-bit), so
// all four targets are checked native vs lifted at -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress26RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress26RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress26RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress26RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress26RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress26RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress26RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress26RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress26TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // u64 union read/written via its two dwords and individual bytes.
    {p+"_pun64",
     t+" "+p+"_pun64("+t+" a){\n"
     "  union { unsigned long long q; unsigned d[2]; unsigned char b[8]; } v;\n"
     "  unsigned s=(unsigned)a|1u; unsigned h=0;\n"
     "  for(int i=0;i<40;i++){ s=s*1103515245u+12345u;\n"
     "    v.q=(unsigned long long)s*0x9E3779B97F4A7C15ull;\n"
     "    v.d[0]=v.d[0]^(v.d[1]+ (unsigned)i);\n"
     "    v.b[3]=(unsigned char)(v.b[3]+v.b[7]);\n"
     "    h=h*131u+v.d[0]+v.d[1]+(unsigned)v.b[3]; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x57ULL}, "OptStress26", 2},

    // Array of u64 unions, runtime-indexed dword/byte sub-lane access.
    {p+"_pun64idx",
     t+" "+p+"_pun64idx("+t+" a){\n"
     "  union { unsigned long long q; unsigned d[2]; unsigned char b[8]; } arr[4];\n"
     "  unsigned s=(unsigned)a|1u; unsigned h=0;\n"
     "  for(int k=0;k<4;k++){ s=s*1103515245u+12345u; arr[k].q=(unsigned long long)s*0x100000001ull; }\n"
     "  for(int i=0;i<48;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned k=(s>>5)&3u;\n"
     "    arr[k].d[(s>>3)&1u]+=s;\n"
     "    arr[k].b[(s>>7)&7u]=(unsigned char)(s>>9);\n"
     "    h=h*131u+arr[k].d[0]+arr[k].d[1]; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0xa3ULL}, "OptStress26", 2},

    // u64 whose low and high dwords evolve under different operations.
    {p+"_mixacc64",
     t+" "+p+"_mixacc64("+t+" a){\n"
     "  union { unsigned long long q; unsigned d[2]; } v; v.q=(unsigned long long)(unsigned)a|1ull;\n"
     "  unsigned s=(unsigned)a|1u; unsigned h=0;\n"
     "  for(int i=0;i<48;i++){ s=s*1103515245u+12345u;\n"
     "    v.d[0]=v.d[0]*1664525u+1013904223u;\n"
     "    v.d[1]=(v.d[1]^(v.d[1]>>13))+s;\n"
     "    v.q+=(v.q<<7)^(v.q>>9);\n"
     "    h=h*131u+v.d[0]+v.d[1]; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x39ULL}, "OptStress26", 2},

    // Swap the two dwords of a u64 and byte-reverse, then read back wide.
    {p+"_punflip",
     t+" "+p+"_punflip("+t+" a){\n"
     "  union { unsigned long long q; unsigned d[2]; unsigned char b[8]; } v;\n"
     "  unsigned s=(unsigned)a|1u; unsigned h=0;\n"
     "  for(int i=0;i<40;i++){ s=s*1103515245u+12345u;\n"
     "    v.q=(unsigned long long)s*0xD6E8FEB86659FD93ull;\n"
     "    unsigned t0=v.d[0]; v.d[0]=v.d[1]; v.d[1]=t0;\n"
     "    for(int k=0;k<4;k++){ unsigned char tb=v.b[k]; v.b[k]=v.b[7-k]; v.b[7-k]=tb; }\n"
     "    h=h*131u+(unsigned)(v.q^(v.q>>32)); }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x6eULL}, "OptStress26", 2},

    // {lo,hi} dword struct used as a manual 64-bit add accumulator.
    {p+"_structq",
     t+" "+p+"_structq("+t+" a){\n"
     "  struct { unsigned lo, hi; } v; v.lo=(unsigned)a|1u; v.hi=(unsigned)a^0x55u;\n"
     "  unsigned s=(unsigned)a|1u; unsigned h=0;\n"
     "  for(int i=0;i<48;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned nl=v.lo+s; unsigned carry=(nl<v.lo)?1u:0u;\n"
     "    v.lo=nl; v.hi=v.hi+(s>>16)+carry;\n"
     "    h=h*131u+v.lo+v.hi; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x84ULL}, "OptStress26", 2},

    // Bitfield struct whose fields straddle the 32-bit word boundary.
    {p+"_bf64",
     t+" "+p+"_bf64("+t+" a){\n"
     "  struct { unsigned long long x:20, y:24, z:20; } v;\n"
     "  unsigned s=(unsigned)a|1u; unsigned h=0;\n"
     "  v.x=s; v.y=s>>3; v.z=s>>7;\n"
     "  for(int i=0;i<48;i++){ s=s*1103515245u+12345u;\n"
     "    v.x=v.x+ (s&0xfffffu);\n"
     "    v.y=v.y^ (s>>4);\n"
     "    v.z=v.z+ v.x;\n"
     "    h=h*131u+(unsigned)v.x+(unsigned)v.y+(unsigned)v.z; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x1bULL}, "OptStress26", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress26TC("x64o26", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress26TC("x86o26", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress26TC("a64o26", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress26TC("armo26", "int");

INSTANTIATE_TEST_SUITE_P(OptStress26, X64OptStress26RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress26, X86OptStress26RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress26, A64OptStress26RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress26, ARM32OptStress26RT, ::testing::ValuesIn(kARM), rtTCName);
