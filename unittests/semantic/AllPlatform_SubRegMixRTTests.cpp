//===- AllPlatform_SubRegMixRTTests.cpp - sub-register aliasing -*-C++*-=//
//
// Sub-register / mixed-width aliasing has historically hidden optimizer-driven
// lift bugs (e.g. an x86 write of RAX then read of AL folded to 0 because the
// AL/RAX alias was lost).  These probes hammer that surface across all four
// targets: bytes assembled into a word then read back, truncation chains
// (u64->u32->u16->u8 with arithmetic at each width), wrapping accumulators kept
// in u8/u16, interleaved sign/zero extension, mask-based bitfield extract/insert
// at odd widths, and 16-bit multiplies that write a partial register.
//
// Every kernel is integer-only, folds to a single integer return, and lowers to
// no runtime helper, so all four targets are checked native vs lifted at -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SubRegMixRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SubRegMixRT, Verify) { roundTripX64(GetParam()); }
class X86SubRegMixRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86SubRegMixRT, Verify) { roundTripX86(GetParam()); }
class A64SubRegMixRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64SubRegMixRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32SubRegMixRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32SubRegMixRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeSubRegMixTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Bytes split out of a word and reassembled, then narrow reads.
    {p+"_asm",
     t+" "+p+"_asm("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<24;i++){ x=x*1103515245u+12345u;\n"
     "    unsigned char b0=(unsigned char)x,b1=(unsigned char)(x>>8),\n"
     "      b2=(unsigned char)(x>>16),b3=(unsigned char)(x>>24);\n"
     "    unsigned w=((unsigned)b3<<24)|((unsigned)b2<<16)|((unsigned)b1<<8)|b0;\n"
     "    unsigned short lo=(unsigned short)w,hi=(unsigned short)(w>>16);\n"
     "    h += (unsigned)(unsigned short)(lo+hi)+(unsigned)(unsigned char)(b0+b3); }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x4cULL}, "SubRegMix", 2},

    // Truncation chain u32->u16->u8 with signed/unsigned arithmetic at each step.
    {p+"_trunc",
     t+" "+p+"_trunc("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<24;i++){ x=x*1103515245u+12345u;\n"
     "    unsigned u32=x; unsigned short u16=(unsigned short)u32;\n"
     "    unsigned char u8=(unsigned char)u16;\n"
     "    int s8=(signed char)u8, s16=(short)u16, s32=(int)u32;\n"
     "    h += (unsigned)(u32+u16+u8) ^ (unsigned)(s8+s16+s32); }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x9bULL}, "SubRegMix", 2},

    // Wrapping accumulators kept in u8 / u16 (partial-register adds).
    {p+"_wracc",
     t+" "+p+"_wracc("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned char acc8=0; unsigned short acc16=0;\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<40;i++){ x=x*1103515245u+12345u;\n"
     "    acc8=(unsigned char)(acc8+(unsigned char)(x>>5));\n"
     "    acc16=(unsigned short)(acc16+(unsigned short)(x>>3));\n"
     "    h=h*131u+(unsigned)acc8+(unsigned)acc16; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0xa7ULL}, "SubRegMix", 2},

    // Interleaved sign extension from byte / halfword lanes.
    {p+"_signext",
     t+" "+p+"_signext("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; int acc=0;\n"
     "  for(int i=0;i<32;i++){ x=x*1103515245u+12345u;\n"
     "    signed char sb=(signed char)x; short sh=(short)(x>>8);\n"
     "    acc += (int)sb*3 - (int)sh + ((int)(signed char)(x>>16)<<1); }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x35ULL}, "SubRegMix", 2},

    // Bitfield pack / unpack at odd widths via masks and shifts.
    {p+"_bitfield",
     t+" "+p+"_bitfield("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<24;i++){ x=x*1103515245u+12345u;\n"
     "    unsigned f1=(x>>3)&7u,f2=(x>>6)&0x1fu,f3=(x>>11)&0x3ffu,f4=(x>>20)&0x1fu;\n"
     "    unsigned packed=(f3<<13)|(f2<<8)|(f1<<5)|f4;\n"
     "    unsigned g1=(packed>>5)&7u,g2=(packed>>8)&0x1fu,g3=(packed>>13)&0x3ffu;\n"
     "    h=h*131u+g1+g2*7u+g3*131u; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x6dULL}, "SubRegMix", 2},

    // 16-bit multiplies that write only a partial register.
    {p+"_mul16",
     t+" "+p+"_mul16("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<24;i++){ x=x*1103515245u+12345u;\n"
     "    unsigned short up=(unsigned short)((unsigned short)(x>>4)*(unsigned short)(x>>13));\n"
     "    short sp=(short)((short)(x>>2)*(short)(x>>17));\n"
     "    h += (unsigned)up + (unsigned)(unsigned short)sp; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x13ULL}, "SubRegMix", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeSubRegMixTC("x64srm", "long");
static const std::vector<RoundTripTC> kX86 = makeSubRegMixTC("x86srm", "int");
static const std::vector<RoundTripTC> kA64 = makeSubRegMixTC("a64srm", "long");
static const std::vector<RoundTripTC> kARM = makeSubRegMixTC("armsrm", "int");

INSTANTIATE_TEST_SUITE_P(SubRegMix, X64SubRegMixRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(SubRegMix, X86SubRegMixRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(SubRegMix, A64SubRegMixRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(SubRegMix, ARM32SubRegMixRT, ::testing::ValuesIn(kARM), rtTCName);
