//===- X86_32_PicRodataRoundTripTests.cpp - i386 PIC constant pool -*- C++ -*-=//
//
// i386 has no EIP-relative addressing, so -O2 clang reaches its vectorized
// constant pool (and other rodata) through the PIC get-PC idiom:
//   call $+5; pop reg; add reg,_GLOBAL_OFFSET_TABLE_; movdqa LCPI@GOTOFF(reg)
// (R_386_GOTPC + R_386_GOTOFF).  These kernels auto-vectorize so clang emits
// that sequence; a roundtrip therefore exercises get-PC thunk recognition, the
// i386 PIC relocations, and rodata-global redirection end to end.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X86PicRT : public SemanticRoundTripFixture,
                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86PicRT, Verify) { roundTripX86(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX86Pic = {
  // Vectorizable int array build + reduce: clang emits a vector constant pool
  // reached via the get-PC thunk (the original failure mode).
  {"x86_vecarr",
   "int x86_vecarr(int a){ int v[16]; for(int i=0;i<16;i++) v[i]=a*(i+1)-i;\n"
   "  int s=0; for(int i=0;i<16;i++) s+=v[i]^(v[(i*7)&15]); return s; }\n",
   {0x53ULL}, "X86Pic", 2, ""},

  // Byte buffer with a vectorizable affine fill, then a hash reduce.
  {"x86_vecbyte",
   "int x86_vecbyte(int a){ unsigned char b[32]; for(int i=0;i<32;i++) b[i]=(unsigned char)(a+i*3);\n"
   "  unsigned h=2166136261u; for(int i=0;i<32;i++){ h^=b[i]; h*=16777619u; } return (int)h; }\n",
   {0x7BULL}, "X86Pic", 2, ""},

  // abs() over a vectorizable array (constant-pool sign masks).
  {"x86_vecabs",
   "int x86_vecabs(int a){ int v[32]; for(int i=0;i<32;i++) v[i]=(a*i)^(i*i);\n"
   "  int s=0; for(int i=0;i<32;i++){ int x=v[i]; s+=(x<0)?-x:x; } return s; }\n",
   {0x39ULL}, "X86Pic", 2, ""},

  // Saturating-ish min/max reduce over a vectorizable array.
  {"x86_vecminmax",
   "int x86_vecminmax(int a){ int v[24]; for(int i=0;i<24;i++) v[i]=a*7-i*131;\n"
   "  int mn=v[0], mx=v[0]; for(int i=1;i<24;i++){ if(v[i]<mn)mn=v[i]; if(v[i]>mx)mx=v[i]; }\n"
   "  return mx-mn; }\n",
   {0x15ULL}, "X86Pic", 2, ""},

  // Multiply-by-constant column (vectorized imul + constant-pool factors).
  {"x86_vecscale",
   "int x86_vecscale(int a){ int v[20]; for(int i=0;i<20;i++) v[i]=(a+i)*1103515245+12345;\n"
   "  long s=0; for(int i=0;i<20;i++) s+=(unsigned)v[i]>>3; return (int)s; }\n",
   {0x61ULL}, "X86Pic", 2, ""},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(X86Pic, X86PicRT, ::testing::ValuesIn(kX86Pic),
                         rtTCName);
