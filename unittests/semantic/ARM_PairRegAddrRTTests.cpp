//===- ARM_PairRegAddrRTTests.cpp - pair / register-indexed addressing -C++-===//
//
// Addressing modes adjacent to the #390 post-index fix that the C path rarely
// emits: AArch64 LDP/STP pair load/store with pre- and post-index writeback,
// ARM32 LDM/STM with base writeback, and ARM32 REGISTER post-index
// (`ldr Rt,[Rn],Rm`) where the writeback adds a register rather than an
// immediate.  Inline asm pins each form; a buggy address/writeback returns the
// wrong element.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64PairAddrRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64PairAddrRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32PairAddrRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32PairAddrRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kA64 = {
  // ldp post-index (64-bit pair): reads b[0..3] then advances by 16 each.
  {"ldp_post",
   "long f(long a){ unsigned long b[8]; for(int i=0;i<8;i++) b[i]=(unsigned long)(a+i*10);\n"
   "  unsigned long* p=b; unsigned long x0,x1,x2,x3;\n"
   "  __asm__ volatile(\"ldp %0,%1,[%4],#16\\n\\tldp %2,%3,[%4],#16\"\n"
   "    :\"=&r\"(x0),\"=&r\"(x1),\"=&r\"(x2),\"=&r\"(x3),\"+r\"(p)::\"memory\");\n"
   "  return (long)(x0*1000+x1*100+x2*10+x3); }\n",
   {0x111ULL}, "PairAddr"},
  // ldp pre-index: address IS base+16 before loading.
  {"ldp_pre",
   "long f(long a){ unsigned long b[8]; for(int i=0;i<8;i++) b[i]=(unsigned long)(a+i*10);\n"
   "  unsigned long* p=b; unsigned long x0,x1,x2,x3;\n"
   "  __asm__ volatile(\"ldp %0,%1,[%4,#16]!\\n\\tldp %2,%3,[%4,#16]!\"\n"
   "    :\"=&r\"(x0),\"=&r\"(x1),\"=&r\"(x2),\"=&r\"(x3),\"+r\"(p)::\"memory\");\n"
   "  return (long)(x0*1000+x1*100+x2*10+x3); }\n",
   {0x222ULL}, "PairAddr"},
  // stp post-index then read back.
  {"stp_post",
   "long f(long a){ unsigned long b[8]={0,0,0,0,0,0,0,0}; unsigned long* p=b;\n"
   "  unsigned long v0=(unsigned long)a,v1=(unsigned long)(a+1),v2=(unsigned long)(a+2),v3=(unsigned long)(a+3);\n"
   "  __asm__ volatile(\"stp %1,%2,[%0],#16\\n\\tstp %3,%4,[%0],#16\"\n"
   "    :\"+r\"(p):\"r\"(v0),\"r\"(v1),\"r\"(v2),\"r\"(v3):\"memory\");\n"
   "  return (long)(b[0]*1000+b[1]*100+b[2]*10+b[3]); }\n",
   {0x333ULL}, "PairAddr"},
  // ldp 32-bit pair post-index (8-byte stride).
  {"ldp_w_post",
   "long f(long a){ unsigned b[12]; for(int i=0;i<12;i++) b[i]=(unsigned)(a+i*10);\n"
   "  unsigned* p=b; unsigned x0,x1,x2,x3;\n"
   "  __asm__ volatile(\"ldp %w0,%w1,[%4],#8\\n\\tldp %w2,%w3,[%4],#8\"\n"
   "    :\"=&r\"(x0),\"=&r\"(x1),\"=&r\"(x2),\"=&r\"(x3),\"+r\"(p)::\"memory\");\n"
   "  return (long)(x0*1000u+x1*100u+x2*10u+x3); }\n",
   {0x444ULL}, "PairAddr"},
};

static const std::vector<RoundTripTC> kARM = {
  // ldm with writeback: loads consecutive words, base advances by list size.
  {"ldm_wb",
   "int f(int a){ unsigned b[12]; for(int i=0;i<12;i++) b[i]=(unsigned)(a+i*10);\n"
   "  unsigned* p=b; unsigned x0,x1,x2;\n"
   "  __asm__ volatile(\"ldm %3!,{%0,%1,%2}\"\n"
   "    :\"=&r\"(x0),\"=&r\"(x1),\"=&r\"(x2),\"+r\"(p)::\"memory\");\n"
   "  return (int)(x0 + x1 + x2 + (unsigned)(p - b)); }\n",
   {0x55ULL}, "PairAddr"},
  // stm with writeback then read back + base delta.
  {"stm_wb",
   "int f(int a){ unsigned b[12]={0,0,0,0,0,0,0,0,0,0,0,0}; unsigned* p=b;\n"
   "  unsigned v0=(unsigned)a,v1=(unsigned)(a+7),v2=(unsigned)(a+9);\n"
   "  __asm__ volatile(\"stm %0!,{%1,%2,%3}\"\n"
   "    :\"+r\"(p):\"r\"(v0),\"r\"(v1),\"r\"(v2):\"memory\");\n"
   "  return (int)(b[0]+b[1]+b[2]+(unsigned)(p-b)); }\n",
   {0x66ULL}, "PairAddr"},
  // Register post-index: ldr Rt,[Rn],Rm advances base by a register (8 bytes).
  {"ldr_reg_post",
   "int f(int a){ unsigned b[16]; for(int i=0;i<16;i++) b[i]=(unsigned)(a+i*10);\n"
   "  unsigned* p=b; unsigned step=8, x0,x1,x2;\n"
   "  __asm__ volatile(\"ldr %0,[%3],%4\\n\\tldr %1,[%3],%4\\n\\tldr %2,[%3],%4\"\n"
   "    :\"=&r\"(x0),\"=&r\"(x1),\"=&r\"(x2),\"+r\"(p):\"r\"(step):\"memory\");\n"
   "  return (int)(x0*10000u+x1*100u+x2); }\n",
   {0x77ULL}, "PairAddr"},
  // Register post-index with scaled register (ldr Rt,[Rn],Rm,lsl#2).
  {"ldr_reg_post_lsl",
   "int f(int a){ unsigned b[20]; for(int i=0;i<20;i++) b[i]=(unsigned)(a+i*10);\n"
   "  unsigned* p=b; unsigned step=2, x0,x1,x2;\n"
   "  __asm__ volatile(\"ldr %0,[%3],%4,lsl#2\\n\\tldr %1,[%3],%4,lsl#2\\n\\tldr %2,[%3],%4,lsl#2\"\n"
   "    :\"=&r\"(x0),\"=&r\"(x1),\"=&r\"(x2),\"+r\"(p):\"r\"(step):\"memory\");\n"
   "  return (int)(x0*10000u+x1*100u+x2); }\n",
   {0x88ULL}, "PairAddr"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(PairAddr, A64PairAddrRT,
                         ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(PairAddr, ARM32PairAddrRT,
                         ::testing::ValuesIn(kARM), rtTCName);
