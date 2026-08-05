//===- X86_32_AlgoRTTests.cpp - i386 algorithm-level probes -----*- C++ -*-===//
//
// The large clang -O2 algorithm probe suites (VectorAlgo*, KernelAlgo, BitAlgo,
// HashAlgo, ...) only run on x86_64 / aarch64 / arm32 — i386 (32-bit x86) has
// no algorithm-level roundtrip coverage despite being a first-class target with
// a distinct register file, cdecl ABI (stack args, EAX/EDX:EAX returns) and
// EDX:EAX-based mul/div modelling.  These kernels are known-good on x86_64, so
// any divergence here is an i386-specific lift/optimizer bug: heavy register
// spilling, partial-register aliasing, EDX:EAX mul/div, and the cdecl frame all
// get exercised at once.  Returns fold to an exact 32-bit value in EAX.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X86_32AlgoRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86_32AlgoRT, Verify) { roundTripX86(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX86 = {
  // Reflected byte CRC-32 (shift / xor with conditional polynomial).
  {"crc32b",
   "unsigned f(unsigned a){\n"
   "  unsigned char buf[64]; for(int i=0;i<64;i++) buf[i]=(unsigned char)(a*(i+1)+i*7);\n"
   "  unsigned crc=0xFFFFFFFFu;\n"
   "  for(int i=0;i<64;i++){ crc^=buf[i];\n"
   "    for(int k=0;k<8;k++) crc=(crc>>1)^(0xEDB88320u&(unsigned)(-(int)(crc&1))); }\n"
   "  return crc^0xFFFFFFFFu;\n"
   "}\n",
   {0x1234567ULL}, "X86_32Algo", 2},

  // FNV-1a hash (multiply + xor mixing).
  {"fnv1a",
   "unsigned f(unsigned a){\n"
   "  unsigned char buf[80]; for(int i=0;i<80;i++) buf[i]=(unsigned char)(a*(i+1)+i*3);\n"
   "  unsigned h=2166136261u;\n"
   "  for(int i=0;i<80;i++){ h^=buf[i]; h*=16777619u; }\n"
   "  return h;\n"
   "}\n",
   {0x2233445ULL}, "X86_32Algo", 2},

  // Bit-by-bit integer square root (shift / compare / conditional subtract).
  {"isqrt",
   "unsigned f(unsigned a){\n"
   "  unsigned acc=0;\n"
   "  for(int k=0;k<40;k++){ unsigned n=(a*7u+k*131u)&0xFFFFFFu, res=0, bit=1u<<22;\n"
   "    while(bit>n) bit>>=2;\n"
   "    while(bit){ if(n>=res+bit){ n-=res+bit; res=(res>>1)+bit; } else res>>=1; bit>>=2; }\n"
   "    acc=acc*131u+res; }\n"
   "  return acc;\n"
   "}\n",
   {0x3344556ULL}, "X86_32Algo", 2},

  // 32-bit LCG PRNG mixing (loop-carried mul-add).
  {"lcg",
   "unsigned f(unsigned a){\n"
   "  unsigned s=a|1u,acc=0;\n"
   "  for(int i=0;i<200;i++){ s=s*1664525u+1013904223u; acc=acc*131u+(s>>24); }\n"
   "  return acc;\n"
   "}\n",
   {0x4455667ULL}, "X86_32Algo", 2},

  // Euclidean GCD chain (modulo loop -> DIV with EDX:EAX).
  {"gcd",
   "unsigned f(unsigned a){\n"
   "  unsigned acc=0;\n"
   "  for(int k=1;k<=64;k++){ unsigned x=(a*k)|1u, y=(a*7u+k*13u)|1u;\n"
   "    while(y){ unsigned t=x%y; x=y; y=t; }\n"
   "    acc=acc*131u+x; }\n"
   "  return acc;\n"
   "}\n",
   {0x5566778ULL}, "X86_32Algo", 2},

  // Population count (Kernighan bit clear loop).
  {"popcount",
   "unsigned f(unsigned a){\n"
   "  unsigned acc=0;\n"
   "  for(int i=0;i<128;i++){ unsigned v=a*(i+1)+i*5,c=0;\n"
   "    while(v){ v&=v-1; c++; } acc=acc*131u+c; }\n"
   "  return acc;\n"
   "}\n",
   {0x6677889ULL}, "X86_32Algo", 2},

  // 64-bit MAC from 32-bit multiplies (EDX:EAX widening multiply chain).
  {"widemul",
   "unsigned f(unsigned a){\n"
   "  unsigned long long acc=0;\n"
   "  for(unsigned i=0;i<128;i++){ unsigned x=a*(i+1)+i, y=a*3u+i*7u;\n"
   "    acc+=(unsigned long long)x*y; }\n"
   "  return (unsigned)(acc^(acc>>32));\n"
   "}\n",
   {0x778899AULL}, "X86_32Algo", 2},

  // Signed division + modulo (IDIV with sign-extended EDX:EAX).
  {"divmod",
   "unsigned f(unsigned a){\n"
   "  unsigned acc=0;\n"
   "  for(int k=1;k<=80;k++){ int x=(int)(a*k)-1000000, d=(int)(k*7-200);\n"
   "    if(d==0)d=1; int q=x/d, r=x%d; acc=acc*131u+(unsigned)(q*3+r); }\n"
   "  return acc;\n"
   "}\n",
   {0x88990ABULL}, "X86_32Algo", 2},

  // Running min/max with argmin/argmax (paired compare-select).
  {"minmaxidx",
   "unsigned f(unsigned a){\n"
   "  unsigned acc=0;\n"
   "  for(int k=0;k<32;k++){ int x[40];\n"
   "    for(int i=0;i<40;i++) x[i]=(int)((a*(i+1)+k*13)&0x1FFFF)-65536;\n"
   "    int mn=x[0],mx=x[0],mni=0,mxi=0;\n"
   "    for(int i=1;i<40;i++){ if(x[i]<mn){mn=x[i];mni=i;} if(x[i]>mx){mx=x[i];mxi=i;} }\n"
   "    acc=acc*131u+(unsigned)(mn+mx+mni*7+mxi*3); }\n"
   "  return acc;\n"
   "}\n",
   {0x99AABBCULL}, "X86_32Algo", 2},

  // Multi-word (96-bit) addition carry chain (ADC propagation).
  {"carrychain",
   "unsigned f(unsigned a){\n"
   "  unsigned w[3]={a,a*3u,a*7u}; unsigned acc=0;\n"
   "  for(int k=0;k<200;k++){ unsigned add=a*(k+1)+k; unsigned carry=add;\n"
   "    for(int j=0;j<3;j++){ unsigned long long s=(unsigned long long)w[j]+carry;\n"
   "      w[j]=(unsigned)s; carry=(unsigned)(s>>32); }\n"
   "    acc=acc*131u+(w[0]^w[1]^w[2]); }\n"
   "  return acc;\n"
   "}\n",
   {0xAABBCCDULL}, "X86_32Algo", 2},

  // Variable-count shifts with flag consumption (SHL/SHR/SAR + setcc).
  {"varshift",
   "unsigned f(unsigned a){\n"
   "  unsigned acc=0;\n"
   "  for(int i=0;i<200;i++){ unsigned v=a*(i+1)+i; unsigned sh=(a+i)&31u;\n"
   "    unsigned lo=v<<sh, hi=v>>sh; int sar=((int)v)>>(sh&15u);\n"
   "    acc=acc*131u+(lo^hi^(unsigned)sar); }\n"
   "  return acc;\n"
   "}\n",
   {0x1357913ULL}, "X86_32Algo", 2},

  // Bitfield extract/insert pack (shift/mask/or steering).
  {"bitfield",
   "unsigned f(unsigned a){\n"
   "  unsigned acc=0;\n"
   "  for(int i=0;i<200;i++){ unsigned v=a*(i+1)+i*3;\n"
   "    unsigned f1=(v>>3)&0x1F, f2=(v>>11)&0x7F, f3=(v>>20)&0xFFF;\n"
   "    unsigned pk=(f3<<19)|(f2<<12)|(f1<<7)|(v&0x7F);\n"
   "    acc=acc*131u+pk; }\n"
   "  return acc;\n"
   "}\n",
   {0x2468ACEULL}, "X86_32Algo", 2},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(X86_32Algo, X86_32AlgoRT, ::testing::ValuesIn(kX86),
                         rtTCName);
