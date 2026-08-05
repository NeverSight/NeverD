//===- AllPlatform_VectorAlgo22RTTests.cpp - SIMD integer kernels -*- C++ -*=//
//
// Twenty-second batch of clang -O2 algorithm probes, weighted toward integer
// SIMD shapes the auto-vectorizer reaches for — saturating add/sub, byte
// packing/narrowing, interleave/deinterleave, cross-lane reduction, shuffle
// permutation, widening multiply-accumulate, and population-count scans.  Each
// kernel folds its result into one exact integer so any lane drop, saturation
// miscompile, narrow/widen width error, or shuffle index slip surfaces:
//   * satacc8   - signed 8-bit saturating add/sub accumulation.
//   * usatadd   - unsigned byte saturating add (clamp at 255).
//   * pack16to8 - saturating narrow 16->8 then byte reduce.
//   * interleave- interleave two byte streams (zip) then hash.
//   * deinterl  - deinterleave (unzip) even/odd lanes.
//   * crossred  - cross-lane sum/xor reduction of a 16-lane vector.
//   * permute   - fixed shuffle permutation by a table of indices.
//   * wmac      - widening 16x16->32 multiply-accumulate.
//   * popscan   - per-byte popcount then prefix combine.
//   * absdiff   - sum of absolute byte differences (SAD).
//
// All arithmetic is bounded and divisor-free, so nothing lowers to a libcall.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo22RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo22RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo22RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo22RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo22RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo22RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeVA22TC(const char *prefix, const char *T,
                                           int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Signed 8-bit saturating add/sub accumulation.
    {p+"_satacc8",
     t+" "+p+"_satacc8("+t+" a){\n"
     "  signed char v[96]; for(int i=0;i<96;i++) v[i]=(signed char)((a*(i+1)+i*5)&0xFF);\n"
     "  unsigned h=0; int acc=0;\n"
     "  for(int i=0;i<96;i++){ acc+=v[i]; if(acc>127)acc=127; if(acc<-128)acc=-128;\n"
     "    h=h*131u+(unsigned)(acc&0xFF); }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo22", opt, fl},

    // Unsigned byte saturating add (clamp at 255).
    {p+"_usatadd",
     t+" "+p+"_usatadd("+t+" a){\n"
     "  unsigned char x[80],y[80];\n"
     "  for(int i=0;i<80;i++){ x[i]=(unsigned char)(a*(i+1)); y[i]=(unsigned char)(a*7+i*9); }\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<80;i++){ unsigned s=(unsigned)x[i]+y[i]; if(s>255)s=255;\n"
     "    h=h*131u+s; }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo22", opt, fl},

    // Saturating narrow 16->8 (clamp to unsigned byte) then reduce.
    {p+"_pack16to8",
     t+" "+p+"_pack16to8("+t+" a){\n"
     "  short v[64]; for(int i=0;i<64;i++) v[i]=(short)((a*(i+2))%900)-300;\n"
     "  unsigned char o[64];\n"
     "  for(int i=0;i<64;i++){ int x=v[i]; o[i]=(unsigned char)(x<0?0:(x>255?255:x)); }\n"
     "  unsigned h=0; for(int i=0;i<64;i++) h=h*131u+o[i];\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo22", opt, fl},

    // Interleave (zip) two byte streams.
    {p+"_interleave",
     t+" "+p+"_interleave("+t+" a){\n"
     "  unsigned char x[32],y[32],o[64];\n"
     "  for(int i=0;i<32;i++){ x[i]=(unsigned char)(a+i); y[i]=(unsigned char)(a*3+i*2); }\n"
     "  for(int i=0;i<32;i++){ o[2*i]=x[i]; o[2*i+1]=y[i]; }\n"
     "  unsigned h=0; for(int i=0;i<64;i++) h=h*131u+o[i];\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo22", opt, fl},

    // Deinterleave (unzip) even/odd lanes.
    {p+"_deinterl",
     t+" "+p+"_deinterl("+t+" a){\n"
     "  unsigned char s[64],ev[32],od[32];\n"
     "  for(int i=0;i<64;i++) s[i]=(unsigned char)(a*(i+1)+i*3);\n"
     "  for(int i=0;i<32;i++){ ev[i]=s[2*i]; od[i]=s[2*i+1]; }\n"
     "  unsigned h=0; for(int i=0;i<32;i++) h=h*131u+(unsigned)(ev[i]*256+od[i]);\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo22", opt, fl},

    // Cross-lane sum + xor reduction of 16-int blocks.
    {p+"_crossred",
     t+" "+p+"_crossred("+t+" a){\n"
     "  unsigned h=0;\n"
     "  for(int blk=0;blk<8;blk++){ unsigned v[16],s=0,x=0;\n"
     "    for(int i=0;i<16;i++) v[i]=(unsigned)(a*(blk*16+i+1)+i*7);\n"
     "    for(int i=0;i<16;i++){ s+=v[i]; x^=v[i]; }\n"
     "    h=h*131u+(s^x); }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo22", opt, fl},

    // Fixed shuffle permutation by a reversed-index table.
    {p+"_permute",
     t+" "+p+"_permute("+t+" a){\n"
     "  int v[32],o[32];\n"
     "  for(int i=0;i<32;i++) v[i]=(int)(a*(i+1)+i);\n"
     "  for(int i=0;i<32;i++) o[i]=v[31-i];\n"
     "  unsigned h=0; for(int i=0;i<32;i++) h=h*131u+(unsigned)o[i];\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo22", opt, fl},

    // Widening 16x16->32 multiply-accumulate.
    {p+"_wmac",
     t+" "+p+"_wmac("+t+" a){\n"
     "  short x[48],y[48];\n"
     "  for(int i=0;i<48;i++){ x[i]=(short)((a+i)%200-100); y[i]=(short)((a*3+i)%150-75); }\n"
     "  long long acc=0;\n"
     "  for(int i=0;i<48;i++) acc+=(int)x[i]*(int)y[i];\n"
     "  return ("+t+")(unsigned)(acc^(acc>>32));\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo22", opt, fl},

    // Per-byte popcount then prefix combine.
    {p+"_popscan",
     t+" "+p+"_popscan("+t+" a){\n"
     "  unsigned char v[128]; for(int i=0;i<128;i++) v[i]=(unsigned char)(a*(i+1)+i*11);\n"
     "  unsigned h=0,run=0;\n"
     "  for(int i=0;i<128;i++){ unsigned c=__builtin_popcount(v[i]); run+=c;\n"
     "    h=h*131u+(run^c); }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x99AABBCULL}, "VectorAlgo22", opt, fl},

    // Sum of absolute byte differences (SAD), blocked.
    {p+"_absdiff",
     t+" "+p+"_absdiff("+t+" a){\n"
     "  unsigned char A[64],B[64]; for(int i=0;i<64;i++){ A[i]=(unsigned char)(a*(i+1)); B[i]=(unsigned char)(a*5+i*7); }\n"
     "  unsigned h=0;\n"
     "  for(int b=0;b<4;b++){ unsigned s=0;\n"
     "    for(int i=0;i<16;i++){ int d=A[b*16+i]-B[b*16+i]; s+=(unsigned)(d<0?-d:d); }\n"
     "    h=h*131u+s; }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0xAABBCCDULL}, "VectorAlgo22", opt, fl},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeVA22TC("x64v22", "long", 2, "");
static const std::vector<RoundTripTC> kA64 = makeVA22TC("a64v22", "long", 2, "");
static const std::vector<RoundTripTC> kARM = makeVA22TC("armv22", "int", 2, "");

INSTANTIATE_TEST_SUITE_P(VectorAlgo22, X64VectorAlgo22RT,
                         ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo22, A64VectorAlgo22RT,
                         ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo22, ARM32VectorAlgo22RT,
                         ::testing::ValuesIn(kARM), rtTCName);
