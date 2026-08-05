//===- X86_32_SSEVectorRTTests.cpp - i386 SSE2 vector probes ----*- C++ -*-===//
//
// i386 -O2 auto-vectorizes integer loops into SSE2 packed code (pmuludq /
// pmaddwd / psadbw / packssdw / punpck...), but the large vector probe suites
// only feed those kernels to x86_64 / aarch64 / arm32.  The 32-bit SSE2 lowering
// differs from x86_64 (8 XMM regs, 32-bit addressing, cdecl spills), so it
// deserves direct roundtrip coverage of the packed paths that previously hid
// real lift bugs (PMULUDQ widening, PMADDWD, PACKSSDW saturation, PUNPCK
// shuffles).  Kernels are known-good on x86_64; any divergence is i386-specific.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X86_32SSEVecRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86_32SSEVecRT, Verify) { roundTripX86(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX86 = {
  // Sliding SAD search (psadbw / pabsd + reduce).
  {"sadsearch",
   "unsigned f(unsigned a){\n"
   "  unsigned char A[64],B[64];\n"
   "  for(int i=0;i<64;i++){ A[i]=(unsigned char)(a*(i+1)+i*3); B[i]=(unsigned char)(a*5+i*7); }\n"
   "  unsigned acc=0;\n"
   "  for(int off=0;off<8;off++){ unsigned s=0;\n"
   "    for(int i=0;i<56;i++){ int d=A[i]-B[i+off]; s+=(unsigned)(d<0?-d:d); }\n"
   "    acc=acc*131u+s; }\n"
   "  return acc;\n"
   "}\n",
   {0x1234567ULL}, "X86_32SSE", 2},

  // Diagnostic: same kernel, NeverD MedIR transforms only (LLVM opt off).
  {"sadsearch_noopt",
   "unsigned f(unsigned a){\n"
   "  unsigned char A[64],B[64];\n"
   "  for(int i=0;i<64;i++){ A[i]=(unsigned char)(a*(i+1)+i*3); B[i]=(unsigned char)(a*5+i*7); }\n"
   "  unsigned acc=0;\n"
   "  for(int off=0;off<8;off++){ unsigned s=0;\n"
   "    for(int i=0;i<56;i++){ int d=A[i]-B[i+off]; s+=(unsigned)(d<0?-d:d); }\n"
   "    acc=acc*131u+s; }\n"
   "  return acc;\n"
   "}\n",
   {0x1234567ULL}, "X86_32SSE", 2, "", /*NoOpt=*/true},

  // Diagnostic: single aligned SAD pass, no offset loop (minimal psadbw).
  {"sad_single",
   "unsigned f(unsigned a){\n"
   "  unsigned char A[64],B[64];\n"
   "  for(int i=0;i<64;i++){ A[i]=(unsigned char)(a*(i+1)+i*3); B[i]=(unsigned char)(a*5+i*7); }\n"
   "  unsigned s=0;\n"
   "  for(int i=0;i<64;i++){ int d=A[i]-B[i]; s+=(unsigned)(d<0?-d:d); }\n"
   "  return s;\n"
   "}\n",
   {0x1234567ULL}, "X86_32SSE", 2},

  // Diagnostic: single SAD pass with FIXED unaligned B offset (+3).
  {"sad_off3",
   "unsigned f(unsigned a){\n"
   "  unsigned char A[64],B[64];\n"
   "  for(int i=0;i<64;i++){ A[i]=(unsigned char)(a*(i+1)+i*3); B[i]=(unsigned char)(a*5+i*7); }\n"
   "  unsigned s=0;\n"
   "  for(int i=0;i<56;i++){ int d=A[i]-B[i+3]; s+=(unsigned)(d<0?-d:d); }\n"
   "  return s;\n"
   "}\n",
   {0x1234567ULL}, "X86_32SSE", 2},

  // Diagnostic: offset loop but return last s only (no acc*131 mixing).
  {"sad_rets",
   "unsigned f(unsigned a){\n"
   "  unsigned char A[64],B[64];\n"
   "  for(int i=0;i<64;i++){ A[i]=(unsigned char)(a*(i+1)+i*3); B[i]=(unsigned char)(a*5+i*7); }\n"
   "  unsigned s=0;\n"
   "  for(int off=0;off<8;off++){ s=0;\n"
   "    for(int i=0;i<56;i++){ int d=A[i]-B[i+off]; s+=(unsigned)(d<0?-d:d); } }\n"
   "  return s;\n"
   "}\n",
   {0x1234567ULL}, "X86_32SSE", 2},

  // Diagnostic: sum all 8 offset SADs (live, but plain add not imul).
  {"sad_sum",
   "unsigned f(unsigned a){\n"
   "  unsigned char A[64],B[64];\n"
   "  for(int i=0;i<64;i++){ A[i]=(unsigned char)(a*(i+1)+i*3); B[i]=(unsigned char)(a*5+i*7); }\n"
   "  unsigned total=0;\n"
   "  for(int off=0;off<8;off++){ unsigned s=0;\n"
   "    for(int i=0;i<56;i++){ int d=A[i]-B[i+off]; s+=(unsigned)(d<0?-d:d); }\n"
   "    total+=s; }\n"
   "  return total;\n"
   "}\n",
   {0x1234567ULL}, "X86_32SSE", 2},

  // Diagnostic: individual fixed offsets to find the culprit lane.
  {"sad_off1",
   "unsigned f(unsigned a){\n"
   "  unsigned char A[64],B[64];\n"
   "  for(int i=0;i<64;i++){ A[i]=(unsigned char)(a*(i+1)+i*3); B[i]=(unsigned char)(a*5+i*7); }\n"
   "  unsigned s=0; for(int i=0;i<56;i++){ int d=A[i]-B[i+1]; s+=(unsigned)(d<0?-d:d); }\n"
   "  return s;\n}\n",
   {0x1234567ULL}, "X86_32SSE", 2},
  {"sad_off7",
   "unsigned f(unsigned a){\n"
   "  unsigned char A[64],B[64];\n"
   "  for(int i=0;i<64;i++){ A[i]=(unsigned char)(a*(i+1)+i*3); B[i]=(unsigned char)(a*5+i*7); }\n"
   "  unsigned s=0; for(int i=0;i<56;i++){ int d=A[i]-B[i+7]; s+=(unsigned)(d<0?-d:d); }\n"
   "  return s;\n}\n",
   {0x1234567ULL}, "X86_32SSE", 2},

  // Per-channel alpha blend (widening mul-add -> pmaddwd / pmullw).
  {"alphablnd",
   "unsigned f(unsigned a){\n"
   "  unsigned acc=0;\n"
   "  for(int i=0;i<128;i++){ unsigned fg=(a*(i+1))&0xFF, bg=(a*7+i*5)&0xFF, al=(a*3+i)&0xFF;\n"
   "    unsigned o=fg*al+bg*(255-al)+128; o=(o+(o>>8))>>8; acc=acc*131u+(o&0xFF); }\n"
   "  return acc;\n"
   "}\n",
   {0x2233445ULL}, "X86_32SSE", 2},

  // Signed int8 widening dot product (pmaddwd accumulation).
  {"dot_i8",
   "unsigned f(unsigned a){\n"
   "  signed char x[128],y[128];\n"
   "  for(int i=0;i<128;i++){ x[i]=(signed char)(a*(i+1)+i); y[i]=(signed char)(a*3+i*5); }\n"
   "  int acc=0; for(int i=0;i<128;i++) acc+=x[i]*y[i];\n"
   "  return (unsigned)acc;\n"
   "}\n",
   {0x3344556ULL}, "X86_32SSE", 2},

  // Signed saturate int32 -> int16 (packssdw / pminsd-pmaxsd clamp).
  {"satpack16",
   "unsigned f(unsigned a){\n"
   "  int x[128]; for(int i=0;i<128;i++) x[i]=(int)((a*(i+1))&0x3FFFF)-131072;\n"
   "  unsigned acc=0;\n"
   "  for(int i=0;i<128;i++){ int v=x[i]; if(v>32767)v=32767; if(v<-32768)v=-32768;\n"
   "    acc=acc*131u+(unsigned)(v&0xFFFF); }\n"
   "  return acc;\n"
   "}\n",
   {0x4455667ULL}, "X86_32SSE", 2},

  // 32-bit element multiply reduce (pmuludq widening path).
  {"mul32reduce",
   "unsigned f(unsigned a){\n"
   "  unsigned x[64],y[64];\n"
   "  for(int i=0;i<64;i++){ x[i]=a*(i+1)+i; y[i]=a*3u+i*7u; }\n"
   "  unsigned long long acc=0; for(int i=0;i<64;i++) acc+=(unsigned long long)x[i]*y[i];\n"
   "  return (unsigned)(acc^(acc>>32));\n"
   "}\n",
   {0x5566778ULL}, "X86_32SSE", 2},

  // Per-element threshold count + masked sum (pcmpgtd + select reduce).
  {"threshold",
   "unsigned f(unsigned a){\n"
   "  unsigned acc=0;\n"
   "  for(int k=0;k<40;k++){ int x[64],th=(int)((a+k)&0xFF);\n"
   "    for(int i=0;i<64;i++) x[i]=(int)((a*(i+1)+k*3)&0xFF);\n"
   "    int cnt=0,sum=0; for(int i=0;i<64;i++) if(x[i]>th){ sum+=x[i]; cnt++; }\n"
   "    acc=acc*131u+(unsigned)(sum+cnt); }\n"
   "  return acc;\n"
   "}\n",
   {0x6677889ULL}, "X86_32SSE", 2},

  // 8x8 byte transpose (punpck shuffles) + reduce.
  {"transp8",
   "unsigned f(unsigned a){\n"
   "  unsigned char m[64],tr[64]; for(int i=0;i<64;i++) m[i]=(unsigned char)(a*(i+1)+i*5);\n"
   "  for(int r=0;r<8;r++) for(int c=0;c<8;c++) tr[c*8+r]=m[r*8+c];\n"
   "  unsigned acc=0; for(int i=0;i<64;i++) acc=acc*131u+(unsigned)(tr[i]+i);\n"
   "  return acc;\n"
   "}\n",
   {0x778899AULL}, "X86_32SSE", 2},

  // Packed min/max reduce over 16-bit lanes (pminsw / pmaxsw).
  {"minmax16",
   "unsigned f(unsigned a){\n"
   "  short x[128]; for(int i=0;i<128;i++) x[i]=(short)(a*(i+1)+i*9);\n"
   "  int mn=32767,mx=-32768; for(int i=0;i<128;i++){ if(x[i]<mn)mn=x[i]; if(x[i]>mx)mx=x[i]; }\n"
   "  return (unsigned)(mx-mn);\n"
   "}\n",
   {0x88990ABULL}, "X86_32SSE", 2},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(X86_32SSE, X86_32SSEVecRT, ::testing::ValuesIn(kX86),
                         rtTCName);
