//===- X64_ScasCmpsRTTests.cpp - single SCAS/CMPS roundtrip ----*- C++ -*-===//
//
// Roundtrip probes for the single (non-repeated) x86 compare/scan string
// instructions SCAS (scasb/scasw/scasl/scasq) and CMPS
// (cmpsb/cmpsw/cmpsl/cmpsq).
//
// These were previously lifted as a non-crashing placeholder: an opaque
// intrinsic was emitted, RSI/RDI/RCX were overwritten with uninitialized
// temporaries and ZF was forced to 0 — so every status flag and the pointer
// advance came out wrong.  SCAS compares the accumulator (AL/AX/EAX/RAX) with
// ES:[RDI]; CMPS compares DS:[RSI] with ES:[RDI].  Both set flags exactly like
// CMP and advance the pointer(s) by the element size.  The single form has no
// memory write and a deterministic advance, so it is now modelled directly in
// MedIR (reusing the CMP flag helper).
//
// Each probe drives the instruction through inline asm (with the architectural
// RSI/RDI/accumulator bindings), reads back the resulting condition codes with
// setcc, and folds the captured flags + pointer delta(s) into the return value
// so the original-vs-recompiled comparison actually depends on the flags and
// the pointer arithmetic.  (REP/REPE/REPNE forms are a documented leftover and
// are not exercised here.)
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64ScasCmpsRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64ScasCmpsRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // scasb, accumulator == ES:[RDI] → ZF=1, CF=0; RDI advances by 1.
  {"scasb_eq",
   "long f(long a){unsigned char buf[4]={(unsigned char)a,0x11,0x22,0x33};"
   "unsigned char*p=buf;unsigned char e=0,b=0,l=0;"
   "__asm__ volatile(\"cld; scasb; sete %0; setb %1; setl %2\""
   ":\"=&q\"(e),\"=&q\"(b),\"=&q\"(l),\"+D\"(p):\"a\"((unsigned char)a)"
   ":\"memory\",\"cc\");"
   "return (long)e+(long)b*2+(long)l*4+(long)(p-buf)*8;}\n",
   {0x5A}, "ScasCmps", 0},

  // scasb, accumulator < ES:[RDI] unsigned (0x10 vs 0x80) → CF=1, ZF=0.
  {"scasb_below",
   "long f(long a){unsigned char buf[4]={0x80,0x11,0x22,0x33};"
   "unsigned char*p=buf;unsigned char e=0,b=0,a_=0,l=0,g=0;"
   "__asm__ volatile(\"cld; scasb; sete %0; setb %1; seta %2; setl %3; setg %4\""
   ":\"=&q\"(e),\"=&q\"(b),\"=&q\"(a_),\"=&q\"(l),\"=&q\"(g),\"+D\"(p)"
   ":\"a\"((unsigned char)a):\"memory\",\"cc\");"
   "return (long)e+(long)b*2+(long)a_*4+(long)l*8+(long)g*16+(long)(p-buf)*32;}\n",
   {0x10}, "ScasCmps", 0},

  // scasb with signed overflow: AL=0x7F (127), [RDI]=0x80 (-128).
  // 127-(-128) overflows the signed byte range → OF set, SF/OF/CF all exercised.
  {"scasb_signed_ovf",
   "long f(long a){unsigned char buf[4]={0x80,0x11,0x22,0x33};"
   "unsigned char*p=buf;unsigned char s=0,o=0,b=0,l=0;"
   "__asm__ volatile(\"cld; scasb; sets %0; seto %1; setb %2; setl %3\""
   ":\"=&q\"(s),\"=&q\"(o),\"=&q\"(b),\"=&q\"(l),\"+D\"(p)"
   ":\"a\"((unsigned char)a):\"memory\",\"cc\");"
   "return (long)s+(long)o*2+(long)b*4+(long)l*8+(long)(p-buf)*16;}\n",
   {0x7F}, "ScasCmps", 0},

  // scasw: 2-byte accumulator vs [RDI]; RDI advances by 2.
  {"scasw",
   "long f(long a){unsigned short buf[4]={0x9000,0x1111,0x2222,0x3333};"
   "unsigned short*p=buf;unsigned char e=0,b=0,l=0;"
   "__asm__ volatile(\"cld; scasw; sete %0; setb %1; setl %2\""
   ":\"=&q\"(e),\"=&q\"(b),\"=&q\"(l),\"+D\"(p):\"a\"((unsigned short)a)"
   ":\"memory\",\"cc\");"
   "return (long)e+(long)b*2+(long)l*4+(long)((char*)p-(char*)buf)*8;}\n",
   {0x1234}, "ScasCmps", 0},

  // scasl (scasd): 4-byte accumulator vs [RDI]; RDI advances by 4.
  {"scasl",
   "long f(long a){unsigned int buf[4]={0x80000000u,0x11111111u,0x22u,0x33u};"
   "unsigned int*p=buf;unsigned char e=0,b=0,l=0;"
   "__asm__ volatile(\"cld; scasl; sete %0; setb %1; setl %2\""
   ":\"=&q\"(e),\"=&q\"(b),\"=&q\"(l),\"+D\"(p):\"a\"((unsigned int)a)"
   ":\"memory\",\"cc\");"
   "return (long)e+(long)b*2+(long)l*4+(long)((char*)p-(char*)buf)*8;}\n",
   {0x12345678}, "ScasCmps", 0},

  // scasq: 8-byte accumulator vs [RDI]; RDI advances by 8.
  {"scasq",
   "long f(long a){unsigned long buf[2]={(unsigned long)a,0x1111111111111111UL};"
   "unsigned long*p=buf;unsigned char e=0,b=0;"
   "__asm__ volatile(\"cld; scasq; sete %0; setb %1\""
   ":\"=&q\"(e),\"=&q\"(b),\"+D\"(p):\"a\"((unsigned long)a):\"memory\",\"cc\");"
   "return (long)e+(long)b*2+(long)((char*)p-(char*)buf)*4;}\n",
   {0xDEADBEEFCAFE1234UL}, "ScasCmps", 0},

  // cmpsb, DS:[RSI] == ES:[RDI] → ZF=1; both pointers advance by 1.
  {"cmpsb_eq",
   "long f(long a){unsigned char s[4]={(unsigned char)a,1,2,3};"
   "unsigned char d[4]={(unsigned char)a,9,9,9};"
   "unsigned char*ps=s,*pd=d;unsigned char e=0,b=0;"
   "__asm__ volatile(\"cld; cmpsb; sete %0; setb %1\""
   ":\"=&q\"(e),\"=&q\"(b),\"+S\"(ps),\"+D\"(pd)::\"memory\",\"cc\");"
   "return (long)e+(long)b*2+(long)(ps-s)*4+(long)(pd-d)*8;}\n",
   {0x44}, "ScasCmps", 0},

  // cmpsb, DS:[RSI] < ES:[RDI] (0x10 vs 0x80) → CF=1, ZF=0.
  {"cmpsb_below",
   "long f(long a){unsigned char s[4]={0x10,1,2,3};unsigned char d[4]={0x80,9,9,9};"
   "unsigned char*ps=s,*pd=d;unsigned char e=0,b=0,a_=0,l=0;"
   "__asm__ volatile(\"cld; cmpsb; sete %0; setb %1; seta %2; setl %3\""
   ":\"=&q\"(e),\"=&q\"(b),\"=&q\"(a_),\"=&q\"(l),\"+S\"(ps),\"+D\"(pd)"
   "::\"memory\",\"cc\");"
   "return (long)e+(long)b*2+(long)a_*4+(long)l*8"
   "+(long)(ps-s)*16+(long)(pd-d)*32;}\n",
   {0x1}, "ScasCmps", 0},

  // cmpsw: 2-byte compare; both pointers advance by 2.
  {"cmpsw",
   "long f(long a){unsigned short s[4]={0x1000,1,2,3};unsigned short d[4]={0x9000,9,9,9};"
   "unsigned short*ps=s,*pd=d;unsigned char e=0,b=0,l=0;"
   "__asm__ volatile(\"cld; cmpsw; sete %0; setb %1; setl %2\""
   ":\"=&q\"(e),\"=&q\"(b),\"=&q\"(l),\"+S\"(ps),\"+D\"(pd)::\"memory\",\"cc\");"
   "return (long)e+(long)b*2+(long)l*4"
   "+(long)((char*)ps-(char*)s)*8+(long)((char*)pd-(char*)d)*16;}\n",
   {0x2}, "ScasCmps", 0},

  // cmpsl (string CMPSD, AT&T `cmpsl`): 4-byte compare; pointers advance by 4.
  {"cmpsl",
   "long f(long a){unsigned int s[4]={0x12345678u,1,2,3};"
   "unsigned int d[4]={0x12345678u,9,9,9};"
   "unsigned int*ps=s,*pd=d;unsigned char e=0,b=0;"
   "__asm__ volatile(\"cld; cmpsl; sete %0; setb %1\""
   ":\"=&q\"(e),\"=&q\"(b),\"+S\"(ps),\"+D\"(pd)::\"memory\",\"cc\");"
   "return (long)e+(long)b*2"
   "+(long)((char*)ps-(char*)s)*4+(long)((char*)pd-(char*)d)*8;}\n",
   {0x3}, "ScasCmps", 0},

  // cmpsl mismatch: ensure the 4-byte compare differs from a byte compare.
  {"cmpsl_ne",
   "long f(long a){unsigned int s[4]={0x00000001u,1,2,3};"
   "unsigned int d[4]={0x80000001u,9,9,9};"
   "unsigned int*ps=s,*pd=d;unsigned char e=0,b=0,l=0;"
   "__asm__ volatile(\"cld; cmpsl; sete %0; setb %1; setl %2\""
   ":\"=&q\"(e),\"=&q\"(b),\"=&q\"(l),\"+S\"(ps),\"+D\"(pd)::\"memory\",\"cc\");"
   "return (long)e+(long)b*2+(long)l*4"
   "+(long)((char*)ps-(char*)s)*8;}\n",
   {0x4}, "ScasCmps", 0},

  // cmpsq: 8-byte compare; both pointers advance by 8.
  {"cmpsq",
   "long f(long a){unsigned long s[2]={(unsigned long)a,1};"
   "unsigned long d[2]={(unsigned long)a+1,9};"
   "unsigned long*ps=s,*pd=d;unsigned char e=0,b=0;"
   "__asm__ volatile(\"cld; cmpsq; sete %0; setb %1\""
   ":\"=&q\"(e),\"=&q\"(b),\"+S\"(ps),\"+D\"(pd)::\"memory\",\"cc\");"
   "return (long)e+(long)b*2"
   "+(long)((char*)ps-(char*)s)*4+(long)((char*)pd-(char*)d)*8;}\n",
   {0x1000}, "ScasCmps", 0},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(ScasCmps, X64ScasCmpsRT, ::testing::ValuesIn(kX64),
                         rtTCName);
