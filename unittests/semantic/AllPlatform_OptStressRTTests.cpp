//===- AllPlatform_OptStressRTTests.cpp - optimizer stress ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Scalar kernels that hammer the NeverD optimizer's most fragile machinery —
// carry-flag SSA chains, condition-flag liveness across branches, sub-register
// aliasing, and select/CMOV survival — historically the richest source of
// silent miscompiles (RCL poison, RAX/AL aliasing, MedFlags chain DCE).  These
// are deliberately NOT auto-vectorizable: they exercise the integer optimizer
// paths that SIMD probes miss.
//   * ipcksum    - ones-complement sum with end-around carry (carry fold).
//   * bignum_add - multi-limb add, carry propagated limb to limb.
//   * bignum_mul - schoolbook 32x32->64 limb multiply with carry.
//   * leb128     - varint encode+decode (shift/mask/continue-bit branches).
//   * bitreader  - MSB-first bit buffer extraction (shift/mask/refill).
//   * longdiv    - restoring long division by repeated subtract (flag chain).
//   * montgomery - modular mul via add-shift-conditional-subtract.
//   * crcbit     - bit-by-bit CRC32 (shift + conditional xor, flag reuse).
//   * gray       - gray<->binary via xor-shift reduction chain.
//   * satmac     - saturating multiply-accumulate with branchy clamps.
//
// All arithmetic is bounded 32-bit (or 32x32->64 limbs, never 64-bit divide so
// ARM32 stays libcall-free); results fold to an exact integer for bit-exact
// native-vs-lifted comparison.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStressRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStressRT, Verify) { roundTripX64(GetParam()); }

class A64OptStressRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStressRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32OptStressRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStressRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptTC(const char *prefix, const char *T,
                                          int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Ones-complement checksum with end-around carry (IP/TCP style).
    {p+"_ipcksum",
     t+" "+p+"_ipcksum("+t+" a){\n"
     "  unsigned w[40];\n"
     "  for(int i=0;i<40;i++) w[i]=(unsigned)(a*(i+1)+i*7)&0xFFFF;\n"
     "  unsigned sum=0;\n"
     "  for(int i=0;i<40;i++){ sum+=w[i];\n"
     "    sum=(sum&0xFFFF)+(sum>>16); }\n"
     "  while(sum>>16) sum=(sum&0xFFFF)+(sum>>16);\n"
     "  return ("+t+")(unsigned short)(~sum);\n"
     "}\n",
     {0x1234567ULL}, "OptStress", opt, fl},

    // Multi-limb bignum add: carry propagated explicitly limb to limb.
    {p+"_bignum_add",
     t+" "+p+"_bignum_add("+t+" a){\n"
     "  unsigned x[16],y[16],z[16];\n"
     "  for(int i=0;i<16;i++){ x[i]=(unsigned)(a*(i+1)*2654435761u);\n"
     "    y[i]=(unsigned)(a*7u+i*40503u)*2246822519u; }\n"
     "  unsigned carry=0;\n"
     "  for(int i=0;i<16;i++){ unsigned long s=(unsigned long)x[i]+y[i]+carry;\n"
     "    z[i]=(unsigned)s; carry=(unsigned)(s>>32); }\n"
     "  unsigned acc=carry;\n"
     "  for(int i=0;i<16;i++) acc=acc*131u+z[i];\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2233445ULL}, "OptStress", opt, fl},

    // Schoolbook 32x32->64 limb multiply with carry accumulation.
    {p+"_bignum_mul",
     t+" "+p+"_bignum_mul("+t+" a){\n"
     "  unsigned x[8],y[8]; unsigned long z[16];\n"
     "  for(int i=0;i<8;i++){ x[i]=(unsigned)(a*(i+1)+i);\n"
     "    y[i]=(unsigned)(a*3u+i*5u); }\n"
     "  for(int i=0;i<16;i++) z[i]=0;\n"
     "  for(int i=0;i<8;i++){ unsigned long carry=0;\n"
     "    for(int j=0;j<8;j++){\n"
     "      unsigned long cur=z[i+j]+(unsigned long)x[i]*y[j]+carry;\n"
     "      z[i+j]=cur&0xFFFFFFFFu; carry=cur>>32; }\n"
     "    z[i+8]+=carry; }\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<16;i++) acc=acc*131u+(unsigned)z[i];\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3344556ULL}, "OptStress", opt, fl},

    // LEB128 varint encode then decode (continuation-bit branches).
    {p+"_leb128",
     t+" "+p+"_leb128("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int k=0;k<60;k++){ unsigned v=(unsigned)(a*(k+1)+k*131);\n"
     "    unsigned char buf[8]; int n=0;\n"
     "    do{ unsigned char b=v&0x7F; v>>=7; if(v) b|=0x80; buf[n++]=b; }while(v);\n"
     "    unsigned out=0; int sh=0;\n"
     "    for(int i=0;i<n;i++){ out|=(unsigned)(buf[i]&0x7F)<<sh; sh+=7;\n"
     "      if(!(buf[i]&0x80)) break; }\n"
     "    acc=acc*131u+out+(unsigned)n; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x4455667ULL}, "OptStress", opt, fl},

    // MSB-first bit-buffer reader: pull variable-width fields (shift/mask/refill).
    {p+"_bitreader",
     t+" "+p+"_bitreader("+t+" a){\n"
     "  unsigned char data[48];\n"
     "  for(int i=0;i<48;i++) data[i]=(unsigned char)(a*(i+1)+i*3);\n"
     "  unsigned acc=0; unsigned bitpos=0;\n"
     "  for(int k=0;k<50;k++){ unsigned width=1+((a+k)&7);\n"
     "    unsigned val=0;\n"
     "    for(unsigned b=0;b<width;b++){ unsigned byte=bitpos>>3, bit=7-(bitpos&7);\n"
     "      if(byte>=48) byte%=48;\n"
     "      val=(val<<1)|((data[byte]>>bit)&1u); bitpos++; }\n"
     "    acc=acc*131u+val; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x5566778ULL}, "OptStress", opt, fl},

    // Restoring long division by repeated conditional subtract (flag chain).
    {p+"_longdiv",
     t+" "+p+"_longdiv("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int k=0;k<40;k++){ unsigned num=(unsigned)(a*(k+1)+k*777);\n"
     "    unsigned den=1+(((unsigned)(a+k))&0x3F);\n"
     "    unsigned q=0,r=0;\n"
     "    for(int b=31;b>=0;b--){ r=(r<<1)|((num>>b)&1u);\n"
     "      if(r>=den){ r-=den; q|=(1u<<b); } }\n"
     "    acc=acc*131u+q+r; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x6677889ULL}, "OptStress", opt, fl},

    // Montgomery-style modular multiply: add-shift-conditional-subtract.
    {p+"_montgomery",
     t+" "+p+"_montgomery("+t+" a){\n"
     "  unsigned acc=0; unsigned m=0xFFF1u;\n"
     "  for(int k=0;k<40;k++){ unsigned x=(unsigned)(a*(k+1))%m;\n"
     "    unsigned y=(unsigned)(a*3+k)%m; unsigned t2=0;\n"
     "    for(int i=0;i<16;i++){ if((y>>i)&1u) t2+=x;\n"
     "      if(t2&1u) t2+=m; t2>>=1; }\n"
     "    if(t2>=m) t2-=m;\n"
     "    acc=acc*131u+t2; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x778899AULL}, "OptStress", opt, fl},

    // Bit-by-bit CRC32 (reflected): shift + conditional xor, heavy flag reuse.
    {p+"_crcbit",
     t+" "+p+"_crcbit("+t+" a){\n"
     "  unsigned char msg[64];\n"
     "  for(int i=0;i<64;i++) msg[i]=(unsigned char)(a*(i+1)+i*5);\n"
     "  unsigned crc=0xFFFFFFFFu;\n"
     "  for(int i=0;i<64;i++){ crc^=msg[i];\n"
     "    for(int b=0;b<8;b++) crc=(crc&1u)?((crc>>1)^0xEDB88320u):(crc>>1); }\n"
     "  return ("+t+")(crc^0xFFFFFFFFu);\n"
     "}\n",
     {0x88990ABULL}, "OptStress", opt, fl},

    // Gray<->binary round trip via xor-shift reduction chain.
    {p+"_gray",
     t+" "+p+"_gray("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int k=0;k<80;k++){ unsigned v=(unsigned)(a*(k+1)+k*271);\n"
     "    unsigned g=v^(v>>1);\n"
     "    unsigned b=g; b^=b>>16; b^=b>>8; b^=b>>4; b^=b>>2; b^=b>>1;\n"
     "    acc=acc*131u+(g^b); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x99AABBCULL}, "OptStress", opt, fl},

    // Saturating multiply-accumulate with branchy clamps (select survival).
    {p+"_satmac",
     t+" "+p+"_satmac("+t+" a){\n"
     "  unsigned acc=0; int s=0;\n"
     "  for(int i=0;i<128;i++){ int x=(int)((a*(i+1))&0x1FFF)-4096;\n"
     "    int c=(int)((a*5+i)&0x7F)-64;\n"
     "    int prod=x*c;\n"
     "    long t2=(long)s+prod;\n"
     "    if(t2>2000000000L) t2=2000000000L;\n"
     "    if(t2<-2000000000L) t2=-2000000000L;\n"
     "    s=(int)t2;\n"
     "    acc=acc*131u+(unsigned)(s&0xFFFF); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0xAABBCCDULL}, "OptStress", opt, fl},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptTC("x64opt", "long", 2, "");
static const std::vector<RoundTripTC> kA64 = makeOptTC("a64opt", "long", 2, "");
static const std::vector<RoundTripTC> kARM = makeOptTC("armopt", "int", 2, "");

INSTANTIATE_TEST_SUITE_P(OptStress, X64OptStressRT,
                         ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress, A64OptStressRT,
                         ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress, ARM32OptStressRT,
                         ::testing::ValuesIn(kARM), rtTCName);
