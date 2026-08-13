//===- ARM_PostIndexAddrRTTests.cpp - post-indexed addressing --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Post-indexed loads/stores (`ldr Rt,[Rn],#imm`) access memory at the UNMODIFIED
// base and only then add the offset to the base.  capstone reports the increment
// in mem.disp, so the lifter must exclude it from the access address — otherwise
// the offset is double-applied (once to the address, once to the writeback) and
// every access lands one slot past the intended element.  These inline-asm
// probes pin each width and direction against pre-indexed controls (where the
// offset IS part of the address).  A buggy lift returns the next element.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32PostIdxRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32PostIdxRT, Verify) { roundTripARM32(GetParam()); }

class A64PostIdxRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64PostIdxRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kARM = {
  // ldr post-index: first load reads buf[0], not buf[1].
  {"ldr_post",
   "int f(int a){ unsigned b[6]; for(int i=0;i<6;i++) b[i]=(unsigned)(a+i*10);\n"
   "  unsigned* p=b; unsigned x0,x1,x2;\n"
   "  __asm__ volatile(\"ldr %0,[%3],#4\\n\\tldr %1,[%3],#4\\n\\tldr %2,[%3],#4\"\n"
   "    :\"=&r\"(x0),\"=&r\"(x1),\"=&r\"(x2),\"+r\"(p)::\"memory\");\n"
   "  return (int)(x0*10000u+x1*100u+x2); }\n",
   {0x1111ULL}, "PostIdx"},
  // str post-index: store lands at buf[0], buf[1], then read back.
  {"str_post",
   "int f(int a){ unsigned b[6]={0,0,0,0,0,0}; unsigned* p=b;\n"
   "  unsigned v0=(unsigned)a, v1=(unsigned)(a+7), v2=(unsigned)(a+9);\n"
   "  __asm__ volatile(\"str %1,[%0],#4\\n\\tstr %2,[%0],#4\\n\\tstr %3,[%0],#4\"\n"
   "    :\"+r\"(p):\"r\"(v0),\"r\"(v1),\"r\"(v2):\"memory\");\n"
   "  return (int)(b[0]*10000u+b[1]*100u+b[2]); }\n",
   {0x222ULL}, "PostIdx"},
  // ldrb post-index (1-byte stride).
  {"ldrb_post",
   "int f(int a){ unsigned char b[6]; for(int i=0;i<6;i++) b[i]=(unsigned char)(a+i*3);\n"
   "  unsigned char* p=b; unsigned x0,x1,x2;\n"
   "  __asm__ volatile(\"ldrb %0,[%3],#1\\n\\tldrb %1,[%3],#1\\n\\tldrb %2,[%3],#1\"\n"
   "    :\"=&r\"(x0),\"=&r\"(x1),\"=&r\"(x2),\"+r\"(p)::\"memory\");\n"
   "  return (int)(x0*10000u+x1*100u+x2); }\n",
   {0x33ULL}, "PostIdx"},
  // ldrh post-index (2-byte stride).
  {"ldrh_post",
   "int f(int a){ unsigned short b[6]; for(int i=0;i<6;i++) b[i]=(unsigned short)(a+i*5);\n"
   "  unsigned short* p=b; unsigned x0,x1,x2;\n"
   "  __asm__ volatile(\"ldrh %0,[%3],#2\\n\\tldrh %1,[%3],#2\\n\\tldrh %2,[%3],#2\"\n"
   "    :\"=&r\"(x0),\"=&r\"(x1),\"=&r\"(x2),\"+r\"(p)::\"memory\");\n"
   "  return (int)(x0*10000u+x1*100u+x2); }\n",
   {0x44ULL}, "PostIdx"},
  // ldr post-index with NEGATIVE stride (walk backwards).
  {"ldr_post_neg",
   "int f(int a){ unsigned b[6]; for(int i=0;i<6;i++) b[i]=(unsigned)(a+i*10);\n"
   "  unsigned* p=&b[5]; unsigned x0,x1,x2;\n"
   "  __asm__ volatile(\"ldr %0,[%3],#-4\\n\\tldr %1,[%3],#-4\\n\\tldr %2,[%3],#-4\"\n"
   "    :\"=&r\"(x0),\"=&r\"(x1),\"=&r\"(x2),\"+r\"(p)::\"memory\");\n"
   "  return (int)(x0*10000u+x1*100u+x2); }\n",
   {0x55ULL}, "PostIdx"},
  // Pre-indexed control: address IS base+offset, so first load reads buf[1].
  {"ldr_pre",
   "int f(int a){ unsigned b[6]; for(int i=0;i<6;i++) b[i]=(unsigned)(a+i*10);\n"
   "  unsigned* p=b; unsigned x0,x1;\n"
   "  __asm__ volatile(\"ldr %0,[%2,#4]!\\n\\tldr %1,[%2,#4]!\"\n"
   "    :\"=&r\"(x0),\"=&r\"(x1),\"+r\"(p)::\"memory\");\n"
   "  return (int)(x0*100u+x1); }\n",
   {0x66ULL}, "PostIdx"},
};

static const std::vector<RoundTripTC> kA64 = {
  // AArch64 ldr post-index (32-bit view): first load reads buf[0].
  {"ldr_post",
   "long f(long a){ unsigned b[6]; for(int i=0;i<6;i++) b[i]=(unsigned)(a+i*10);\n"
   "  unsigned* p=b; unsigned x0,x1,x2;\n"
   "  __asm__ volatile(\"ldr %w0,[%3],#4\\n\\tldr %w1,[%3],#4\\n\\tldr %w2,[%3],#4\"\n"
   "    :\"=&r\"(x0),\"=&r\"(x1),\"=&r\"(x2),\"+r\"(p)::\"memory\");\n"
   "  return (long)(x0*10000u+x1*100u+x2); }\n",
   {0x1111ULL}, "PostIdx"},
  // AArch64 str post-index.
  {"str_post",
   "long f(long a){ unsigned b[6]={0,0,0,0,0,0}; unsigned* p=b;\n"
   "  unsigned v0=(unsigned)a, v1=(unsigned)(a+7), v2=(unsigned)(a+9);\n"
   "  __asm__ volatile(\"str %w1,[%0],#4\\n\\tstr %w2,[%0],#4\\n\\tstr %w3,[%0],#4\"\n"
   "    :\"+r\"(p):\"r\"(v0),\"r\"(v1),\"r\"(v2):\"memory\");\n"
   "  return (long)(b[0]*10000u+b[1]*100u+b[2]); }\n",
   {0x222ULL}, "PostIdx"},
  // AArch64 ldrb post-index.
  {"ldrb_post",
   "long f(long a){ unsigned char b[6]; for(int i=0;i<6;i++) b[i]=(unsigned char)(a+i*3);\n"
   "  unsigned char* p=b; unsigned x0,x1,x2;\n"
   "  __asm__ volatile(\"ldrb %w0,[%3],#1\\n\\tldrb %w1,[%3],#1\\n\\tldrb %w2,[%3],#1\"\n"
   "    :\"=&r\"(x0),\"=&r\"(x1),\"=&r\"(x2),\"+r\"(p)::\"memory\");\n"
   "  return (long)(x0*10000u+x1*100u+x2); }\n",
   {0x33ULL}, "PostIdx"},
  // AArch64 ldr post-index 64-bit (x register, 8-byte stride).
  {"ldr_post_x",
   "long f(long a){ unsigned long b[6]; for(int i=0;i<6;i++) b[i]=(unsigned long)(a+i*100);\n"
   "  unsigned long* p=b; unsigned long x0,x1,x2;\n"
   "  __asm__ volatile(\"ldr %0,[%3],#8\\n\\tldr %1,[%3],#8\\n\\tldr %2,[%3],#8\"\n"
   "    :\"=&r\"(x0),\"=&r\"(x1),\"=&r\"(x2),\"+r\"(p)::\"memory\");\n"
   "  return (long)(x0*10000u+x1*100u+x2); }\n",
   {0x77ULL}, "PostIdx"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(PostIdx, ARM32PostIdxRT,
                         ::testing::ValuesIn(kARM), rtTCName);
INSTANTIATE_TEST_SUITE_P(PostIdx, A64PostIdxRT,
                         ::testing::ValuesIn(kA64), rtTCName);
