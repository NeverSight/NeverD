//===- AllPlatform_OptStress6RTTests.cpp - codegen-shape stressors -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Sixth batch of high-yield roundtrip probes, distinct from OptStress / 2-5.
// Each kernel is shaped to make clang -O2 select a particular instruction
// family whose lift/optimizer handling is error-prone: a wide value spilled to
// the stack and reloaded at byte/halfword widths with both sign- and
// zero-extension (load width + sub-register aliasing through memory), the same
// operand pair compared as signed AND unsigned with both results combined
// (MedFlags polarity folding), a variable rotate built from guarded shifts so
// it lowers to ROL/ROR with no shift-by-width UB, a runtime divisor whose
// quotient and remainder are both consumed signed and unsigned (div lowering
// that must stay native, never a libcall), a min/max/abs/clamp ladder over
// mixed signedness (SMIN/UMAX/CSEL select trees), runtime-positioned bitfield
// extract+deposit (UBFX/BFI/BEXTR), and a carry/borrow chain through
// __builtin_*_overflow (ADC/SBB flag reuse).  Each returns a value-dependent
// hash compiled at -O2; the roundtrip compares native vs lifted on all four
// targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64Opt6RT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64Opt6RT, Verify) { roundTripX64(GetParam()); }
class X86Opt6RT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86Opt6RT, Verify) { roundTripX86(GetParam()); }
class A64Opt6RT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64Opt6RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32Opt6RT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32Opt6RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOpt6TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Wide value spilled then reloaded at narrower widths with both sign- and
    // zero-extension.  Forces the lifter to model load widths and the optimizer
    // to track sub-register slices through stack memory.
    {p+"_memwidth",
     t+" "+p+"_memwidth("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned h=0;\n"
     "  for(int i=0;i<64;i++){\n"
     "    unsigned buf[2]; buf[0]=x; buf[1]=x*2654435761u;\n"
     "    unsigned char *b=(unsigned char*)buf;\n"
     "    int s8=(int)(signed char)b[(x>>1)&7u];\n"
     "    unsigned z8=b[(x>>4)&7u];\n"
     "    short *hh=(short*)buf;\n"
     "    int s16=(int)hh[(x>>2)&3u];\n"
     "    unsigned z16=(unsigned short)hh[(x>>6)&3u];\n"
     "    h += (unsigned)s8*131u + z8*7u + (unsigned)s16*3u + z16;\n"
     "    x=(x*1664525u+1013904223u)^h; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x1234ULL}, "Opt6", 2},

    // Same operand pair compared as signed and unsigned; both results feed the
    // hash.  The optimizer must keep each condition pinned to its own compare
    // polarity and not fold a signed test onto the unsigned one (or vice versa).
    {p+"_cmppolar",
     t+" "+p+"_cmppolar("+t+" a){\n"
     "  unsigned x=(unsigned)a; unsigned h=0;\n"
     "  for(int i=0;i<64;i++){\n"
     "    unsigned y=x^((unsigned)i*0x9E3779B9u);\n"
     "    int su=( (int)x < (int)y );\n"
     "    int uu=( x < y );\n"
     "    int sge=( (int)x >= (int)y );\n"
     "    int uge=( x >= y );\n"
     "    h += (unsigned)su*131u + (unsigned)uu*7u\n"
     "       + (unsigned)sge*5u + (unsigned)uge*3u;\n"
     "    if(su!=uu) h^=0xABCDu;\n"
     "    x=(x*1664525u+1013904223u)^(y+h); }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x55AAULL}, "Opt6", 2},

    // Variable rotate built from guarded shifts (no shift-by-width).  clang
    // recognises the idiom and emits ROL/ROR with a register count.
    {p+"_varrot",
     t+" "+p+"_varrot("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned h=0;\n"
     "  for(int i=0;i<64;i++){\n"
     "    unsigned r=(x>>3)&31u;\n"
     "    unsigned rl=(x<<r)|(x>>((32u-r)&31u));\n"
     "    unsigned rr=(x>>r)|(x<<((32u-r)&31u));\n"
     "    h += rl*131u + rr*7u + r;\n"
     "    x=(x*1664525u+1013904223u)^(rl^rr); }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0xC3C3ULL}, "Opt6", 2},

    // Runtime divisor (never zero, signed dividend never INT_MIN/-1): quotient
    // and remainder both consumed, signed and unsigned.  Must lower to native
    // div/rem on every target and never widen into a libcall.
    {p+"_divrem",
     t+" "+p+"_divrem("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned h=0;\n"
     "  for(int i=0;i<64;i++){\n"
     "    unsigned d=((x>>5)&0xFFu)|1u;\n"
     "    unsigned uq=x/d, ur=x%d;\n"
     "    int sx=(int)(x&0x7FFFFFFFu); int sd=(int)d;\n"
     "    int sq=sx/sd, sr=sx%sd;\n"
     "    h += uq*131u + ur*7u + (unsigned)sq*3u + (unsigned)sr;\n"
     "    x=(x*1664525u+1013904223u)^(uq^ur); }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0xBEEFULL}, "Opt6", 2},

    // Min/max/abs/clamp ladder over mixed signedness — a select tree the
    // optimizer pairs across compare polarity (SMIN/SMAX/UMIN/UMAX / CSEL).
    {p+"_clampladder",
     t+" "+p+"_clampladder("+t+" a){\n"
     "  int x=(int)a; unsigned h=0;\n"
     "  for(int i=0;i<64;i++){\n"
     "    int ax = x<0 ? -x : x;\n"
     "    int lo=-1000, hi=2000;\n"
     "    int cl = ax<lo?lo:(ax>hi?hi:ax);\n"
     "    unsigned u=(unsigned)x;\n"
     "    unsigned um = u<0x4000u?u:0x4000u;\n"
     "    int smn = x<(int)um?x:(int)um;\n"
     "    int smx = x>(int)um?x:(int)um;\n"
     "    h += (unsigned)ax + (unsigned)cl*131u + um*7u\n"
     "       + (unsigned)smn*3u + (unsigned)smx*5u;\n"
     "    x = (int)(h ^ (unsigned)(i*2654435761u)); }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x7F00ULL}, "Opt6", 2},

    // Runtime-positioned bitfield extract then deposit into a result word.
    // Lowers to UBFX/BFI on AArch64/ARM, BEXTR/shift-mask on x86.
    {p+"_bitfield",
     t+" "+p+"_bitfield("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned h=0;\n"
     "  for(int i=0;i<64;i++){\n"
     "    unsigned pos=(x>>2)&15u, len=((x>>8)&7u)+1u;\n"
     "    unsigned mask=(len>=32u)?0xFFFFFFFFu:((1u<<len)-1u);\n"
     "    unsigned field=(x>>pos)&mask;\n"
     "    unsigned dpos=(x>>11)&15u;\n"
     "    unsigned out=(h & ~(mask<<dpos)) | ((field&mask)<<dpos);\n"
     "    h = out*131u + field*7u + pos + len;\n"
     "    x=(x*1664525u+1013904223u)^out; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x0FF0ULL}, "Opt6", 2},

    // Carry/borrow chain through __builtin_*_overflow: each step's carry feeds
    // the next add and a select, exercising ADC/SBB flag reuse without the
    // optimizer dropping or reordering a flag consumer.
    {p+"_carrychain",
     t+" "+p+"_carrychain("+t+" a){\n"
     "  unsigned x=(unsigned)a; unsigned h=0;\n"
     "  for(int i=0;i<64;i++){\n"
     "    unsigned s1,s2,s3; \n"
     "    unsigned c1=__builtin_add_overflow(x, (unsigned)i*0x9E3779B9u, &s1);\n"
     "    unsigned c2=__builtin_add_overflow(s1, c1, &s2);\n"
     "    unsigned b1=__builtin_sub_overflow(s2, x>>3, &s3);\n"
     "    unsigned sel = (c1|c2) ? (s3*2654435761u) : (s3^0x85EBCA6Bu);\n"
     "    h = h*31u + sel + c1*131u + c2*7u + b1*5u;\n"
     "    x=(x*1664525u+1013904223u)^sel; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0xABCDULL}, "Opt6", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOpt6TC("x64opt6", "long");
static const std::vector<RoundTripTC> kX86 = makeOpt6TC("x86opt6", "int");
static const std::vector<RoundTripTC> kA64 = makeOpt6TC("a64opt6", "long");
static const std::vector<RoundTripTC> kARM = makeOpt6TC("armopt6", "int");

INSTANTIATE_TEST_SUITE_P(Opt6, X64Opt6RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Opt6, X86Opt6RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(Opt6, A64Opt6RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Opt6, ARM32Opt6RT, ::testing::ValuesIn(kARM), rtTCName);
