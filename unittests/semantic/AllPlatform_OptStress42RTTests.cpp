//===- AllPlatform_OptStress42RTTests.cpp - narrow mem + sign mix -*-C++*-=//
//
// Roundtrip probes for the sign/zero-extension + comparison-flag + stack-frame
// interaction: small local byte buffers (alloca) read back at narrow signed and
// unsigned widths, compared with both signed and unsigned predicates, and used
// to drive conditional accumulation.  These exercise the LowToMed sub-register
// extension path (MOVSX/MOVZX / LDRSB/LDRB / SXTB), MedFlags signed-vs-unsigned
// branch selection, and frame modeling together.  Each kernel is a bounded -O2
// loop returning a value-dependent hash; all libcall-free on every target.
//
//   * mixsign  - the same byte read as int8 and uint8, signed `< 0` and unsigned
//                `> 200` branches in one iteration (cmp + js / cmp + ja).
//   * clampb   - saturating clamp to int8 [-128,127] and uint8 [0,255].
//   * memhash  - unaligned 16/32-bit loads from a byte buffer hashed together.
//   * minmax   - running signed min/max with index tracking (cmov chains).
//   * selnet   - a branchless 5-element compare/swap sort network.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress42RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress42RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress42RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress42RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress42RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress42RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress42RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress42RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress42TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // mixsign: same byte read int8 (signed `<0`) and uint8 (unsigned `>200`).
    {p+"_mixsign",
     t+" "+p+"_mixsign("+t+" a){\n"
     "  unsigned char b[16]; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<16;i++){ b[i]=(unsigned char)(s>>(i&7)); s=s*1103515245u+12345u; }\n"
     "  int acc=0;\n"
     "  for(int j=0;j<200;j++){\n"
     "    int idx=(int)((unsigned)(acc+j)&15u);\n"
     "    signed char sv=(signed char)b[idx];\n"
     "    unsigned char uv=b[(idx+1)&15];\n"
     "    if(sv < 0) acc += sv; else acc -= sv;\n"
     "    if(uv > 200u) acc ^= 0x55; else acc += uv;\n"
     "    acc = acc*131 + (sv * (int)uv); }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x61u}, "OptStress42", 2},

    // clampb: saturating clamp to int8 and uint8 ranges.
    {p+"_clampb",
     t+" "+p+"_clampb("+t+" a){\n"
     "  int acc=(int)a|1; unsigned s=(unsigned)a|3u;\n"
     "  for(int j=0;j<200;j++){\n"
     "    int v=(int)(s ^ (s>>11)) - 400;\n"
     "    int sc = v<-128 ? -128 : (v>127 ? 127 : v);\n"
     "    int uc = v<0 ? 0 : (v>255 ? 255 : v);\n"
     "    acc = acc*131 + sc*7 + uc*3;\n"
     "    s = s*1103515245u+12345u + (unsigned)acc; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x62u}, "OptStress42", 2},

    // memhash: unaligned 16/32-bit reads from a byte buffer.
    {p+"_memhash",
     t+" "+p+"_memhash("+t+" a){\n"
     "  unsigned char b[20]; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<20;i++){ b[i]=(unsigned char)(s>>((i*5)&7)); s=s*48271u+1u; }\n"
     "  unsigned acc=0;\n"
     "  for(int j=0;j<160;j++){\n"
     "    int o=(int)((unsigned)(acc+j)%13u);\n"
     "    unsigned u16=(unsigned)(b[o]|(b[o+1]<<8));\n"
     "    unsigned u32=(unsigned)(b[o]|(b[o+1]<<8)|(b[o+2]<<16)|(b[o+3]<<24));\n"
     "    acc = acc*16777619u ^ (u16 + (u32>>3)); }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x63u}, "OptStress42", 2},

    // minmax: running signed min/max with the argmax index (cmov chains).
    {p+"_minmax",
     t+" "+p+"_minmax("+t+" a){\n"
     "  int mn=2000000000, mx=-2000000000, mi=0, acc=0; unsigned s=(unsigned)a|1u;\n"
     "  for(int j=0;j<200;j++){\n"
     "    int v=(int)(s>>2) - 0x40000000;\n"
     "    if(v<mn) mn=v;\n"
     "    if(v>mx){ mx=v; mi=j; }\n"
     "    acc = acc*131 + (mn>>8) + (mx>>8) + mi;\n"
     "    s = s*1103515245u+12345u; }\n"
     "  return ("+t+")(unsigned)(acc ^ mn ^ mx ^ mi); }\n",
     {0x64u}, "OptStress42", 2},

    // selnet: branchless 5-element compare/swap sort network.
    {p+"_selnet",
     t+" "+p+"_selnet("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int acc=0;\n"
     "  for(int j=0;j<160;j++){\n"
     "    int e[5];\n"
     "    for(int k=0;k<5;k++){ e[k]=(int)(s>>4); s=s*1103515245u+12345u; }\n"
     "    #define CS(x,y) do{ int lo=e[x]<e[y]?e[x]:e[y]; int hi=e[x]<e[y]?e[y]:e[x]; e[x]=lo; e[y]=hi; }while(0)\n"
     "    CS(0,1); CS(3,4); CS(2,4); CS(2,3); CS(0,3); CS(0,2); CS(1,4); CS(1,3); CS(1,2);\n"
     "    #undef CS\n"
     "    acc = acc*131 + e[0]-e[2]+e[4]; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x65u}, "OptStress42", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress42TC("x64o42", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress42TC("x86o42", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress42TC("a64o42", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress42TC("armo42", "int");

INSTANTIATE_TEST_SUITE_P(OptStress42, X64OptStress42RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress42, X86OptStress42RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress42, A64OptStress42RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress42, ARM32OptStress42RT, ::testing::ValuesIn(kARM), rtTCName);
