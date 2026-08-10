//===- AllPlatform_VectorAlgo28RTTests.cpp - shift/select/bitfield edges C++==//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Twenty-eighth batch of clang -O2 probes targeting the edge-count and
// data-movement corners that historically broke (RCL/RCR shift-by-bitwidth,
// sub-register extension, flag-fold select webs): variable shift/rotate amounts
// hitting 0 and near-width, signed/unsigned extension chains across widths,
// conditional-select webs (cmov/csel/csinc), variable bitfield extract/insert,
// 32x32->64 high/low multiply accumulation, and count-leading/trailing-zero
// reductions over a stream.  All four targets including i386; no library calls.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo28RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo28RT, Verify) { roundTripX64(GetParam()); }

class X86VectorAlgo28RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86VectorAlgo28RT, Verify) { roundTripX86(GetParam()); }

class A64VectorAlgo28RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo28RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo28RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo28RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeVA28TC(const char *prefix, const char *T,
                                           int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Variable shift amounts cycling through 0..31 (edge counts incl. 0).
    {p+"_varshift",
     t+" "+p+"_varshift("+t+" a){\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<64;i++){ unsigned x=(unsigned)(a+i*2654435761u); unsigned s=(unsigned)i&31u;\n"
     "    unsigned l=x<<s, r=x>>s; int as=(int)x>>s;\n"
     "    h=h*131u+(l^r^(unsigned)as); }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo28", opt, fl},

    // Sign/zero extension chains across widths (sub-register aliasing).
    {p+"_extchain",
     t+" "+p+"_extchain("+t+" a){\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<80;i++){ unsigned v=(unsigned)(a+i*0x9E3779B1u);\n"
     "    signed char sb=(signed char)v; unsigned char ub=(unsigned char)v;\n"
     "    short sh=(short)v; unsigned short uh=(unsigned short)v;\n"
     "    int x=(int)sb + (int)ub + (int)sh + (int)uh;\n"
     "    h=h*131u+(unsigned)x; }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo28", opt, fl},

    // Conditional-select web (nested ternary -> cmov/csel/csinc chains).
    {p+"_selweb",
     t+" "+p+"_selweb("+t+" a){\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<96;i++){ int x=(int)(a+i*7), y=(int)(a*3-i*5), z=(int)(a^(i*131));\n"
     "    int m = x<y ? (y<z?z:y) : (x<z?z:x);\n"
     "    int n = (x^y)<0 ? (z>0?z:-z) : (x>y?x-y:y-x);\n"
     "    int r = (i&1) ? (m>n?m:n) : (m<n?m:n);\n"
     "    h=h*131u+(unsigned)r; }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo28", opt, fl},

    // Variable rotate (build rotate from shifts, amount incl. 0).
    {p+"_rotvar",
     t+" "+p+"_rotvar("+t+" a){\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<64;i++){ unsigned x=(unsigned)(a*(i+1)+i); unsigned s=(unsigned)(i*5)&31u;\n"
     "    unsigned rol = (x<<s)|(x>>((32-s)&31)); unsigned ror=(x>>s)|(x<<((32-s)&31));\n"
     "    if(s==0){ rol=x; ror=x; }\n"
     "    h=h*131u+(rol^ror); }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo28", opt, fl},

    // Variable bitfield extract+insert at runtime positions.
    {p+"_bitfield",
     t+" "+p+"_bitfield("+t+" a){\n"
     "  unsigned acc=(unsigned)a;\n"
     "  for(int i=0;i<72;i++){ unsigned pos=(unsigned)i&15u, wid=((unsigned)(i>>2)&7u)+1u;\n"
     "    unsigned mask=((1u<<wid)-1u);\n"
     "    unsigned field=(acc>>pos)&mask;\n"
     "    acc=(acc&~(mask<<pos))|((field+ (unsigned)i)&mask)<<pos;\n"
     "    acc=acc*2654435761u+field; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo28", opt, fl},

    // 32x32->64 widening multiply, take high and low halves, accumulate.
    {p+"_mulhilo",
     t+" "+p+"_mulhilo("+t+" a){\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<60;i++){ unsigned x=(unsigned)(a+i*40503), y=(unsigned)(a*3+i*7919);\n"
     "    unsigned long long p64=(unsigned long long)x*(unsigned long long)y;\n"
     "    unsigned hi=(unsigned)(p64>>32), lo=(unsigned)p64;\n"
     "    long long sp=(long long)(int)x*(long long)(int)y;\n"
     "    unsigned shi=(unsigned)((unsigned long long)sp>>32);\n"
     "    h=h*131u+(hi^lo^shi); }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo28", opt, fl},

    // Count leading / trailing zeros over a stream (edge: skip zero inputs).
    {p+"_clztz",
     t+" "+p+"_clztz("+t+" a){\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<80;i++){ unsigned x=(unsigned)(a*(i+1))|((unsigned)i+1u);\n"
     "    unsigned lz=0,tz=0,t1=x,t2=x;\n"
     "    while(!(t1&0x80000000u)){ lz++; t1<<=1; if(lz>=32)break; }\n"
     "    while(!(t2&1u)){ tz++; t2>>=1; if(tz>=32)break; }\n"
     "    h=h*131u+(lz*37u+tz); }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo28", opt, fl},

    // Variable signed/unsigned divide+modulo by nonzero runtime divisor.
    {p+"_divmod",
     t+" "+p+"_divmod("+t+" a){\n"
     "  unsigned h=0;\n"
     "  for(int i=1;i<=64;i++){ int d=(int)((a%97)+i); if(d==0)d=1;\n"
     "    int sv=(int)(a*7-i*13); unsigned uv=(unsigned)(a*3+i*11);\n"
     "    int sq=sv/d, sr=sv%d; unsigned uq=uv/(unsigned)(d<0?-d:d), ur=uv%(unsigned)(d<0?-d:d);\n"
     "    h=h*131u+(unsigned)(sq^sr)+(uq^ur); }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo28", opt, fl},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeVA28TC("x64v28", "long", 2, "");
static const std::vector<RoundTripTC> kX86 = makeVA28TC("x86v28", "int", 2, "");
static const std::vector<RoundTripTC> kA64 = makeVA28TC("a64v28", "long", 2, "");
static const std::vector<RoundTripTC> kARM = makeVA28TC("armv28", "int", 2, "");

INSTANTIATE_TEST_SUITE_P(VectorAlgo28, X64VectorAlgo28RT,
                         ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo28, X86VectorAlgo28RT,
                         ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo28, A64VectorAlgo28RT,
                         ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo28, ARM32VectorAlgo28RT,
                         ::testing::ValuesIn(kARM), rtTCName);
