//===- AllPlatform_OptStress7RTTests.cpp - codegen-shape stressors -*-C++*-=//
//
// Seventh batch of high-yield roundtrip probes, with kernels distinct from
// OptStress / 2-6.  Each is shaped to make clang -O2 select an error-prone
// instruction/optimizer interaction: a sign-extended sub-word load used as a
// signed (negative) array index (sub-register sign extension feeding an address),
// a 128-bit add/sub carry chain built from explicit 32-bit limbs (ADC/SBB reuse
// with no soft-float/__int128 libcall), a data-dependent conditional-select chain
// (CMOV/CSEL where each step's predicate consumes the previous result), a
// variable-count funnel shift (SHLD/SHRD or shift-or pair, no shift-by-width UB),
// a 32-bit word assembled from four byte loads then byte-reversed (memory model +
// BSWAP/REV), and INT_MIN negation / abs overflow edges.  Each returns a
// value-dependent hash compiled at -O2; the roundtrip compares native vs lifted
// on all four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64Opt7RT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64Opt7RT, Verify) { roundTripX64(GetParam()); }
class X86Opt7RT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86Opt7RT, Verify) { roundTripX86(GetParam()); }
class A64Opt7RT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64Opt7RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32Opt7RT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32Opt7RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOpt7TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Sign-extended sub-word load used as a signed (possibly negative) array
    // index off a mid-array base: the lifter must sign-extend the byte before it
    // becomes an address offset, and the optimizer must not drop the sign.
    {p+"_sextidx",
     t+" "+p+"_sextidx("+t+" a){\n"
     "  int tab[16]; for(int i=0;i<16;i++) tab[i]=(int)((unsigned)a*(i+1)+i*7);\n"
     "  signed char sel[8]; for(int i=0;i<8;i++) sel[i]=(signed char)((unsigned)a>>(i*3));\n"
     "  int *base=&tab[8]; unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<8;i++){ int idx=(int)(signed char)sel[i] % 8; \n"
     "    h = h*131u + (unsigned)base[idx] + (unsigned)idx*7u; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x12345ULL}, "Opt7", 2},

    // 128-bit add/sub carry chain via explicit 32-bit limbs.  Exercises ADC/SBB
    // flag reuse across four words without any __int128 / soft-float libcall.
    {p+"_wide128",
     t+" "+p+"_wide128("+t+" a){\n"
     "  unsigned x[4],y[4];\n"
     "  for(int i=0;i<4;i++){ x[i]=(unsigned)a*(i+1)+0x9e3779b9u*i; y[i]=(unsigned)a^(0x55u<<i); }\n"
     "  unsigned s[4],d[4]; unsigned long long c=0;\n"
     "  for(int i=0;i<4;i++){ c += (unsigned long long)x[i] + y[i]; s[i]=(unsigned)c; c>>=32; }\n"
     "  long long b=0;\n"
     "  for(int i=0;i<4;i++){ long long t=(long long)x[i] - y[i] - b; d[i]=(unsigned)t; b=(t<0)?1:0; }\n"
     "  unsigned h=(unsigned)c;\n"
     "  for(int i=0;i<4;i++) h=h*131u+s[i]*7u+d[i]*3u;\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0xABCDEULL}, "Opt7", 2},

    // Data-dependent conditional-select chain: each iteration picks one of two
    // updates based on a predicate over the running value (CMOV/CSEL chain whose
    // condition consumes the prior select result).
    {p+"_cselchain",
     t+" "+p+"_cselchain("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned h=0;\n"
     "  for(int i=0;i<48;i++){\n"
     "    unsigned a1=x*2654435761u+1u; unsigned a2=(x^0x9e3779b9u)*131u;\n"
     "    unsigned nx = ((int)x < (int)h) ? a1 : a2;\n"
     "    nx = (nx & 1u) ? (nx + (h>>2)) : (nx ^ (h<<1));\n"
     "    h = h*31u + nx; x = nx; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x6789ULL}, "Opt7", 2},

    // Variable-count funnel shift (concatenate two words, shift by a runtime
    // count, take a window): lowers to SHLD/SHRD on x86, a shift-or pair
    // elsewhere, with no shift-by-width UB (count masked to 1..31).
    {p+"_funnel",
     t+" "+p+"_funnel("+t+" a){\n"
     "  unsigned hi=(unsigned)a|0x80000000u, lo=(unsigned)a*2654435761u; unsigned h=0;\n"
     "  for(int i=0;i<32;i++){\n"
     "    unsigned s=((unsigned)a>>(i&7))&31u; if(!s) s=1u;\n"
     "    unsigned fl=(hi<<s)|(lo>>(32u-s));\n"
     "    unsigned fr=(lo>>s)|(hi<<(32u-s));\n"
     "    h += fl*131u + fr*7u + s; hi=fl; lo=fr; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0xC0FFEULL}, "Opt7", 2},

    // A 32-bit word assembled from four individual byte loads then byte-reversed:
    // the lifter's memory model must keep the four loads and the optimizer/codegen
    // may fuse them into a wide load + BSWAP/REV; both sides must agree.
    {p+"_loadmerge",
     t+" "+p+"_loadmerge("+t+" a){\n"
     "  unsigned char buf[32];\n"
     "  for(int i=0;i<32;i++) buf[i]=(unsigned char)((unsigned)a*(i+3)+i*131);\n"
     "  unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<29;i++){\n"
     "    unsigned w=(unsigned)buf[i]|((unsigned)buf[i+1]<<8)|((unsigned)buf[i+2]<<16)|((unsigned)buf[i+3]<<24);\n"
     "    unsigned r=__builtin_bswap32(w);\n"
     "    h = h*131u + w*7u + r*3u; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0xDEADULL}, "Opt7", 2},

    // INT_MIN negation / abs overflow edges: -x and (x<0?-x:x) at INT_MIN must
    // wrap exactly like the hardware NEG (no UB-driven constant fold divergence).
    {p+"_negabs",
     t+" "+p+"_negabs("+t+" a){\n"
     "  int v[8]={(int)0x80000000,(int)0x80000001,-1,0,1,0x7fffffff,(int)0xC0000000,(int)a};\n"
     "  unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<8;i++){ int x=v[(i+(int)((unsigned)a&7))&7];\n"
     "    unsigned neg=(unsigned)(-x); unsigned ab=(unsigned)(x<0?-x:x);\n"
     "    int sg=(x>0)-(x<0);\n"
     "    h = h*131u + neg*7u + ab*3u + (unsigned)sg; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x5A5AULL}, "Opt7", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOpt7TC("x64opt7", "long");
static const std::vector<RoundTripTC> kX86 = makeOpt7TC("x86opt7", "int");
static const std::vector<RoundTripTC> kA64 = makeOpt7TC("a64opt7", "long");
static const std::vector<RoundTripTC> kARM = makeOpt7TC("armopt7", "int");

INSTANTIATE_TEST_SUITE_P(Opt7, X64Opt7RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Opt7, X86Opt7RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(Opt7, A64Opt7RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Opt7, ARM32Opt7RT, ::testing::ValuesIn(kARM), rtTCName);
