//===- X64_RepStringRTTests.cpp - REP MOVS/STOS roundtrip ------*- C++ -*-===//
//
// Roundtrip probes for the x86 REP string instructions (rep movs/stos), the
// canonical inlined memcpy/memset idioms.  These were previously lifted as a
// stub: the intrinsic was emitted with no operands and RSI/RDI/RCX were
// overwritten with uninitialized temporaries, so the memory effect was lost
// entirely and the recompiled `rep` ran with garbage pointers/count.
//
// Each probe drives the instruction through inline asm with the architectural
// register bindings (RSI=src, RDI=dst, RCX=count, AL/AX/EAX/RAX=store value)
// and reads back the affected memory so the original-vs-recompiled comparison
// actually depends on the copy/fill.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64RepStringRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64RepStringRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // rep stosq: fill the first 2 of 4 qwords with the value, leave the rest.
  {"rep_stosq",
   "long f(long a){unsigned long b[4];b[0]=b[1]=b[2]=b[3]=0x1111111111111111UL;"
   "unsigned long *d=b;unsigned long n=2;"
   "__asm__ volatile(\"cld; rep stosq\":\"+D\"(d),\"+c\"(n):\"a\"((unsigned long)a):\"memory\",\"cc\");"
   "return (long)(b[0]+b[2]);}\n",
   {0x2233445566778899ULL}, "RepString", 0},

  // rep stosb: fill the first 8 of 16 bytes with the low byte of the value.
  {"rep_stosb",
   "long f(long a){unsigned char b[16];for(int i=0;i<16;i++)b[i]=0x55;"
   "unsigned char *d=b;unsigned long n=8;"
   "__asm__ volatile(\"cld; rep stosb\":\"+D\"(d),\"+c\"(n):\"a\"((unsigned char)a):\"memory\",\"cc\");"
   "return (long)b[2]+(long)b[10]*256;}\n",
   {0x42}, "RepString", 0},

  // rep stosd: fill the first 3 of 4 dwords with the low 32 bits.  (AT&T spells
  // the dword string op `stosl`.)
  {"rep_stosd",
   "long f(long a){unsigned int b[4];b[0]=b[1]=b[2]=b[3]=0x33333333u;"
   "unsigned int *d=b;unsigned long n=3;"
   "__asm__ volatile(\"cld; rep stosl\":\"+D\"(d),\"+c\"(n):\"a\"((unsigned int)a):\"memory\",\"cc\");"
   "return (long)b[0]+(long)b[3];}\n",
   {0xDEADBEEFULL}, "RepString", 0},

  // rep movsq: copy the first 3 of 4 qwords from src to dst.
  {"rep_movsq",
   "long f(long a){unsigned long s[4]={(unsigned long)a,(unsigned long)a+1,"
   "(unsigned long)a+2,(unsigned long)a+3};unsigned long d[4]={9,9,9,9};"
   "unsigned long *ps=s,*pd=d;unsigned long n=3;"
   "__asm__ volatile(\"cld; rep movsq\":\"+S\"(ps),\"+D\"(pd),\"+c\"(n)::\"memory\",\"cc\");"
   "return (long)(d[0]+d[2]+d[3]);}\n",
   {0x1000ULL}, "RepString", 0},

  // rep movsb: copy the first 10 of 16 bytes from src to dst.
  {"rep_movsb",
   "long f(long a){unsigned char s[16];unsigned char d[16];"
   "for(int i=0;i<16;i++){s[i]=(unsigned char)(a+i);d[i]=0;}"
   "unsigned char *ps=s,*pd=d;unsigned long n=10;"
   "__asm__ volatile(\"cld; rep movsb\":\"+S\"(ps),\"+D\"(pd),\"+c\"(n)::\"memory\",\"cc\");"
   "return (long)d[0]+(long)d[9]+(long)d[10];}\n",
   {0x30}, "RepString", 0},

  // rep movsd: copy the first 2 of 4 dwords from src to dst.  (AT&T spells the
  // dword string op `movsl`.)
  {"rep_movsd",
   "long f(long a){unsigned int s[4]={(unsigned int)a,(unsigned int)a+1,"
   "(unsigned int)a+2,(unsigned int)a+3};unsigned int d[4]={7,7,7,7};"
   "unsigned int *ps=s,*pd=d;unsigned long n=2;"
   "__asm__ volatile(\"cld; rep movsl\":\"+S\"(ps),\"+D\"(pd),\"+c\"(n)::\"memory\",\"cc\");"
   "return (long)d[0]+(long)d[1]+(long)d[2];}\n",
   {0x5000ULL}, "RepString", 0},

  // Single (non-REP) stosb: one byte store, pointer advances by 1.
  {"stosb_single",
   "long f(long a){unsigned char b[4]={0x77,0x77,0x77,0x77};"
   "unsigned char *d=b;"
   "__asm__ volatile(\"cld; stosb\":\"+D\"(d):\"a\"((unsigned char)a):\"memory\",\"cc\");"
   "return (long)b[0]+(long)b[1]*256+(long)(d-b)*65536;}\n",
   {0x42}, "RepString", 0},

  // Single lodsb: AL = [RSI]; RSI advances by 1.  Verifies the load value and
  // the pointer increment (the old stub left both undefined).
  {"lodsb_single",
   "long f(long a){unsigned char b[4]={(unsigned char)a,(unsigned char)(a+1),"
   "(unsigned char)(a+2),(unsigned char)(a+3)};unsigned char *s=b;unsigned char v;"
   "__asm__ volatile(\"cld; lodsb\":\"=a\"(v),\"+S\"(s)::\"memory\");"
   "return (long)v+(long)(s-b)*256;}\n",
   {0x40}, "RepString", 0},

  // Single lodsl (lodsd): EAX = [RSI]; RSI advances by 4.  (AT&T spells the
  // dword string op `lodsl`.)
  {"lodsd_single",
   "long f(long a){unsigned int b[4]={(unsigned)a,(unsigned)a+1,(unsigned)a+2,"
   "(unsigned)a+3};unsigned int *s=b;unsigned int v;"
   "__asm__ volatile(\"cld; lodsl\":\"=a\"(v),\"+S\"(s)::\"memory\");"
   "return (long)v+(long)((unsigned char*)s-(unsigned char*)b)*0x100000000L;}\n",
   {0xCAFE}, "RepString", 0},

  // lodsl zero-extends EAX into RAX (clears the upper 32 bits).  Pre-load RAX
  // with all-ones; after lodsl the upper half must be zero.
  {"lodsd_zext_rax",
   "long f(long a){unsigned int b[2]={(unsigned)a,0};unsigned int *s=b;"
   "unsigned long rax;"
   "__asm__ volatile(\"movq $-1,%%rax; cld; lodsl; movq %%rax,%0\""
   ":\"=r\"(rax),\"+S\"(s)::\"rax\",\"memory\");return (long)rax;}\n",
   {0xABCD}, "RepString", 0},

  // Single lodsq: RAX = [RSI]; RSI advances by 8.
  {"lodsq_single",
   "long f(long a){unsigned long b[2]={(unsigned long)a*0x100000001UL,0};"
   "unsigned long *s=b;unsigned long v;"
   "__asm__ volatile(\"cld; lodsq\":\"=a\"(v),\"+S\"(s)::\"memory\");"
   "return (long)v;}\n",
   {0x1234}, "RepString", 0},

  // rep lodsb: loops over RCX bytes; only the last survives in AL.  Verifies the
  // last-element semantics, the pointer advance, and RCX zeroing.
  {"rep_lodsb",
   "long f(long a){unsigned char b[4]={(unsigned char)a,(unsigned char)(a+1),"
   "(unsigned char)(a+2),(unsigned char)(a+3)};unsigned char *s=b;"
   "unsigned long n=4;unsigned char v;"
   "__asm__ volatile(\"cld; rep lodsb\":\"=a\"(v),\"+S\"(s),\"+c\"(n)::\"memory\",\"cc\");"
   "return (long)v+(long)(s-b)*256+(long)n*65536;}\n",
   {0x40}, "RepString", 0},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(RepString, X64RepStringRT, ::testing::ValuesIn(kX64),
                         rtTCName);
