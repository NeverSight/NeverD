//===- AllPlatform_BitFieldRTTests.cpp - C bitfield roundtrip --*-C++*-=//
//
// C bitfields compile to bit-precise extract/insert sequences: x86 lowers them
// to shift + and + or, while AArch64/ARM32 use the dedicated UBFX/SBFX/BFI/BFM
// family.  Read-modify-write of a packed field is exactly the sub-register merge
// idiom (`(word & ~fieldmask) | (newbits << shift)`) but at arbitrary widths and
// offsets that cross byte boundaries -- a stress test for width-accurate mask /
// shift lifting, signed narrow-field sign extension, and narrow load/store of
// fields held in memory.  Each kernel mutates fields in a loop and returns a
// single integer mix, native vs lifted, on all four targets at -O2.
//
//   * bf_pack   - four unsigned fields (5/7/11/9) read-modified-written in a loop
//   * bf_signed - signed bitfields (6/13/13); reads must sign-extend the field
//   * bf_span   - odd widths (3/10/6/13) straddling byte boundaries
//   * bf_flags  - 1-bit flags + wide counters with branches gating field updates
//   * bf_mem    - an array of bitfield structs indexed in memory (narrow ld/st)
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64BitFieldRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64BitFieldRT, Verify) { roundTripX64(GetParam()); }
class X86BitFieldRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86BitFieldRT, Verify) { roundTripX86(GetParam()); }
class A64BitFieldRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64BitFieldRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32BitFieldRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32BitFieldRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeBitFieldTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Four packed unsigned fields, each read-modified-written every iteration:
    // exercises insert at offsets 0/5/12/23 with widths 5/7/11/9.
    {p+"_pack",
     t+" "+p+"_pack("+t+" a){\n"
     "  struct S{ unsigned x:5; unsigned y:7; unsigned z:11; unsigned w:9; } s;\n"
     "  unsigned u=(unsigned)a|1u;\n"
     "  s.x=u; s.y=u>>3; s.z=u>>7; s.w=u>>13;\n"
     "  for(int i=0;i<100;i++){ u=u*1103515245u+12345u;\n"
     "    s.x=s.x+u; s.y=s.y^(u>>5); s.z=s.z+(u>>9); s.w=s.w-(u>>2); }\n"
     "  return ("+t+")(unsigned)((s.x*131u)^(s.y<<5)^(s.z<<11)^(s.w<<20)); }\n",
     {0x4d2ULL}, "BitField", 2},

    // Signed bitfields: reading s.x/s.y/s.z must sign-extend from 6/13/13 bits,
    // so a wrong field width changes the signed accumulation.
    {p+"_signed",
     t+" "+p+"_signed("+t+" a){\n"
     "  struct S{ int x:6; int y:13; int z:13; } s;\n"
     "  unsigned u=(unsigned)a|1u; int acc=0;\n"
     "  s.x=(int)u; s.y=(int)(u>>4); s.z=(int)(u>>10);\n"
     "  for(int i=0;i<100;i++){ u=u*1103515245u+12345u;\n"
     "    s.x=(int)(s.x+(int)u); s.y=(int)(s.y-(int)(u>>3)); s.z=(int)(s.z^(int)(u>>7));\n"
     "    acc += s.x + s.y - s.z; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x39ULL}, "BitField", 2},

    // Odd widths straddling byte boundaries (3/10/6/13) at offsets 0/3/13/19.
    {p+"_span",
     t+" "+p+"_span("+t+" a){\n"
     "  struct S{ unsigned p:3; unsigned q:10; unsigned r:6; unsigned s:13; } v;\n"
     "  unsigned u=(unsigned)a|1u;\n"
     "  v.p=u; v.q=u>>2; v.r=u>>9; v.s=u>>14;\n"
     "  for(int i=0;i<80;i++){ u=u*1103515245u+12345u;\n"
     "    v.p=v.p+1u; v.q=v.q*3u+(u>>5); v.r=v.r^(u>>2); v.s=v.s+(u>>6); }\n"
     "  return ("+t+")(unsigned)(v.p ^ (v.q<<3) ^ (v.r<<13) ^ (v.s<<19)); }\n",
     {0x66ULL}, "BitField", 2},

    // 1-bit flags + wide counters, with branches gating which field is updated.
    {p+"_flags",
     t+" "+p+"_flags("+t+" a){\n"
     "  struct S{ unsigned f0:1; unsigned f1:1; unsigned cnt:14; unsigned tag:16; } v;\n"
     "  unsigned u=(unsigned)a|1u;\n"
     "  v.f0=u; v.f1=u>>1; v.cnt=u>>2; v.tag=u>>16;\n"
     "  for(int i=0;i<120;i++){ u=u*1103515245u+12345u;\n"
     "    v.f0 ^= (u>>3)&1u; v.f1 = v.f0 ^ (u>>5);\n"
     "    if(v.f0) v.cnt=(v.cnt+1u); if(v.f1) v.tag=(v.tag+u); }\n"
     "  return ("+t+")(unsigned)((v.f0)|(v.f1<<1)|(v.cnt<<2)|(v.tag<<16)); }\n",
     {0x77ULL}, "BitField", 2},

    // Array of bitfield structs indexed in memory: each field is a narrow load /
    // store at a computed address with a mask insert.
    {p+"_mem",
     t+" "+p+"_mem("+t+" a){\n"
     "  struct S{ unsigned a:12; unsigned b:20; } arr[4];\n"
     "  unsigned u=(unsigned)a|1u;\n"
     "  for(int i=0;i<4;i++){ arr[i].a=u; arr[i].b=u>>3; u=u*1103515245u+12345u; }\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<60;i++){ u=u*1103515245u+12345u; int j=(int)(u&3u);\n"
     "    arr[j].a=arr[j].a+u; arr[j].b=arr[j].b^(u>>4);\n"
     "    acc += arr[j].a + arr[j].b; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x88ULL}, "BitField", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeBitFieldTC("x64bf", "long");
static const std::vector<RoundTripTC> kX86 = makeBitFieldTC("x86bf", "int");
static const std::vector<RoundTripTC> kA64 = makeBitFieldTC("a64bf", "long");
static const std::vector<RoundTripTC> kARM = makeBitFieldTC("armbf", "int");

INSTANTIATE_TEST_SUITE_P(BitField, X64BitFieldRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(BitField, X86BitFieldRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(BitField, A64BitFieldRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(BitField, ARM32BitFieldRT, ::testing::ValuesIn(kARM), rtTCName);
