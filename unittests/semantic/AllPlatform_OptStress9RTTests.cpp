//===- AllPlatform_OptStress9RTTests.cpp - opt-stress probes ----*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Optimizer-stress roundtrip probes in shapes the OptStress1-8 series did not
// reach, chosen to exercise paths that have historically hidden optimizer /
// lift semantic bugs:
//
//   * satacc  - a 64-bit-wide saturating accumulator clamped to the 32-bit
//               range (carry + 64-bit compare + select + truncate; on i386 /
//               arm32 the i64 intermediate lowers to add/adc + branchless
//               clamp, never a libcall).
//   * net6    - a branchless 6-element sorting network (min/max ternaries lower
//               to a cmov / csel cluster).
//   * hexhash - nibble->ASCII hex through a 16-byte rodata table with byte
//               stores into a stack buffer and byte reloads (constant-pool
//               mapping + sub-register byte aliasing).
//   * rotmix  - runtime-variable rotates plus arithmetic / logical shifts by a
//               data-dependent amount (shift-count masking, divergent across
//               x86 / ARM).
//   * carry96 - a 96-bit (three 32-bit limb) add carry chain written by hand
//               (carry-flag modeling through compare-derived carries).
//   * signmag - branchless conditional negate / abs via arithmetic-shift sign
//               masks (sign mask + xor/sub idioms).
//
// Every kernel is integer-only, folds to a single integer return, and lowers
// to no runtime helper, so all four targets are checked native vs lifted at
// -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress9RT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress9RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress9RT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress9RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress9RT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress9RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress9RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress9RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress9TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 64-bit saturating accumulator clamped to the int32 range.
    {p+"_satacc",
     t+" "+p+"_satacc("+t+" a){\n"
     "  int acc=0; unsigned x=(unsigned)a|1u;\n"
     "  for(int i=0;i<20;i++){\n"
     "    int v=(int)(x&0xffffu)-0x8000;\n"
     "    long long s=(long long)acc+(long long)v*((i&1)?-3:5);\n"
     "    if(s>2147483647LL) s=2147483647LL;\n"
     "    if(s<(-2147483647LL-1)) s=(-2147483647LL-1);\n"
     "    acc=(int)s; x=x*1103515245u+12345u; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x4cULL}, "OptStress9", 2},

    // branchless 6-element sorting network (min/max swaps).
    {p+"_net6",
     t+" "+p+"_net6("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; int e[6];\n"
     "  for(int i=0;i<6;i++){ x=x*1103515245u+12345u; e[i]=(int)((x>>8)&0xffffu)-0x8000; }\n"
     "#define SW(i,j) do{ int lo=e[i]<e[j]?e[i]:e[j]; int hi=e[i]<e[j]?e[j]:e[i]; e[i]=lo; e[j]=hi; }while(0)\n"
     "  SW(0,1);SW(2,3);SW(4,5); SW(0,2);SW(3,5);SW(1,4);\n"
     "  SW(0,1);SW(2,3);SW(4,5); SW(1,2);SW(3,4); SW(2,3);\n"
     "#undef SW\n"
     "  unsigned h=0; for(int i=0;i<6;i++) h=h*131u+(unsigned)e[i];\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x9bULL}, "OptStress9", 2},

    // nibble->ASCII hex through a rodata table, byte stores + reloads.
    {p+"_hexhash",
     t+" "+p+"_hexhash("+t+" a){\n"
     "  static const char hx[17]=\"0123456789abcdef\";\n"
     "  unsigned char buf[16]; unsigned v=(unsigned)a;\n"
     "  for(int i=0;i<8;i++) buf[i]=(unsigned char)hx[(v>>(28-4*i))&15u];\n"
     "  v=v*2654435761u+1u;\n"
     "  for(int i=0;i<8;i++) buf[8+i]=(unsigned char)hx[(v>>(28-4*i))&15u];\n"
     "  unsigned h=0; for(int i=0;i<16;i++) h=h*131u+(unsigned char)buf[i];\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0xa7ULL}, "OptStress9", 2},

    // runtime-variable rotate + arithmetic / logical shift by data amount.
    {p+"_rotmix",
     t+" "+p+"_rotmix("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; int s=(int)a; unsigned h=0;\n"
     "  for(int i=0;i<16;i++){\n"
     "    unsigned k=(((unsigned)a)+(unsigned)i*7u)&31u; if(k==0) k=1;\n"
     "    x=(x<<k)|(x>>(32-k));\n"
     "    int sh=(int)(k&15u);\n"
     "    int sa=s>>sh; unsigned su=((unsigned)a)>>sh;\n"
     "    h=h*131u+x+(unsigned)sa+su; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x35ULL}, "OptStress9", 2},

    // 96-bit (three limb) add carry chain by hand.
    {p+"_carry96",
     t+" "+p+"_carry96("+t+" a){\n"
     "  unsigned lo=(unsigned)a, mid=(unsigned)a^0x9e3779b9u, hi=(unsigned)a*2654435761u;\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<16;i++){\n"
     "    unsigned alo=(unsigned)a*(unsigned)(i+1), amid=(unsigned)a+(unsigned)i*0x1234u, ahi=(unsigned)i*0x77u;\n"
     "    unsigned nlo=lo+alo; unsigned c0=nlo<lo?1u:0u;\n"
     "    unsigned nmid=mid+amid; unsigned c1=nmid<mid?1u:0u; nmid+=c0; c1+=(nmid<c0?1u:0u);\n"
     "    unsigned nhi=hi+ahi+c1;\n"
     "    lo=nlo; mid=nmid; hi=nhi; h=h*131u+lo+mid+hi; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x6dULL}, "OptStress9", 2},

    // branchless conditional negate / abs via arithmetic-shift sign masks.
    {p+"_signmag",
     t+" "+p+"_signmag("+t+" a){\n"
     "  int x=(int)a; unsigned h=0;\n"
     "  for(int i=0;i<16;i++){\n"
     "    int m=x>>31; int ax=(x^m)-m;\n"
     "    int neg=((i&3)==0)?-ax:ax; int sel=(x&1)?neg:ax;\n"
     "    x=(int)((unsigned)x*1103515245u+12345u); h=h*131u+(unsigned)sel; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x13ULL}, "OptStress9", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress9TC("x64o9", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress9TC("x86o9", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress9TC("a64o9", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress9TC("armo9", "int");

INSTANTIATE_TEST_SUITE_P(OptStress9, X64OptStress9RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress9, X86OptStress9RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress9, A64OptStress9RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress9, ARM32OptStress9RT, ::testing::ValuesIn(kARM), rtTCName);
