//===- X64_RepCmpScasRTTests.cpp - REP CMPS/SCAS roundtrip -----*- C++ -*-===//
//
// Roundtrip probes for the REP/REPE/REPNE compare & scan string instructions —
// the inlined `memcmp` / `strlen` / `memchr` idioms.
//
// These used to lift to a placeholder: an opaque intrinsic clobbered RAX,
// RSI/RDI/RCX were overwritten with uninitialized temporaries and ZF was forced
// to 0, so the whole `neverd lift` run also printed "unhandled intrinsic:
// code=1063 (cmpsb)" per occurrence (issue #9).  The loop now lowers to the
// real hardware instruction; its leftover count drives the post-termination
// RSI/RDI/RCX state, and the status flags are recomputed from the last element
// pair the loop compared.
//
// Each probe reads back the affected registers AND the condition codes so the
// original-vs-recompiled comparison depends on both the loop's stopping point
// and its flags.  RCX == 0 on entry is exercised both for register state and
// with controlled incoming flags plus deliberately unmapped RSI/RDI values.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64RepCmpScasRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64RepCmpScasRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // repe cmpsb over two equal buffers: runs to RCX==0 with ZF=1, and both
  // pointers land one past the last element.
  {"repe_cmpsb_equal",
   "long f(long a){unsigned char s[8],d[8];"
   "for(int i=0;i<8;i++){s[i]=(unsigned char)(a+i);d[i]=(unsigned char)(a+i);}"
   "unsigned char*ps=s,*pd=d;unsigned long n=8;unsigned char e=0,b=0;"
   "__asm__ volatile(\"cld; repe cmpsb; sete %0; setb %1\""
   ":\"=&q\"(e),\"=&q\"(b),\"+S\"(ps),\"+D\"(pd),\"+c\"(n)::\"memory\",\"cc\");"
   "return (long)e+(long)b*2+(long)(ps-s)*4+(long)(pd-d)*64+(long)n*1024;}\n",
   {0x21}, "RepCmpScas", 0},

  // repe cmpsb stopping at a mismatch: ZF=0, CF from the mismatching pair, and
  // RCX/RSI/RDI freeze at the element after it.
  {"repe_cmpsb_mismatch",
   "long f(long a){unsigned char s[8],d[8];"
   "for(int i=0;i<8;i++){s[i]=(unsigned char)(a+i);d[i]=(unsigned char)(a+i);}"
   "d[3]=0x80;"
   "unsigned char*ps=s,*pd=d;unsigned long n=8;unsigned char e=0,b=0,g=0;"
   "__asm__ volatile(\"cld; repe cmpsb; sete %0; setb %1; seta %2\""
   ":\"=&q\"(e),\"=&q\"(b),\"=&q\"(g),\"+S\"(ps),\"+D\"(pd),\"+c\"(n)"
   "::\"memory\",\"cc\");"
   "return (long)e+(long)b*2+(long)g*4+(long)(ps-s)*8+(long)(pd-d)*128"
   "+(long)n*2048;}\n",
   {0x11}, "RepCmpScas", 0},

  // The mirrored mismatch (source byte above destination byte) so CF=0/ZF=0 is
  // distinguished from the CF=1 case above rather than both folding to "!=".
  {"repe_cmpsb_above",
   "long f(long a){unsigned char s[8],d[8];"
   "for(int i=0;i<8;i++){s[i]=(unsigned char)(a+i);d[i]=(unsigned char)(a+i);}"
   "s[2]=0x90;d[2]=0x10;"
   "unsigned char*ps=s,*pd=d;unsigned long n=8;unsigned char e=0,b=0,g=0,l=0;"
   "__asm__ volatile(\"cld; repe cmpsb; sete %0; setb %1; seta %2; setl %3\""
   ":\"=&q\"(e),\"=&q\"(b),\"=&q\"(g),\"=&q\"(l),\"+S\"(ps),\"+D\"(pd),\"+c\"(n)"
   "::\"memory\",\"cc\");"
   "return (long)e+(long)b*2+(long)g*4+(long)l*8+(long)(ps-s)*16"
   "+(long)n*4096;}\n",
   {0x31}, "RepCmpScas", 0},

  // repe cmpsl / cmpsw / cmpsq: the element size must drive both the compare
  // width and the pointer stride.
  {"repe_cmpsl",
   "long f(long a){unsigned int s[4],d[4];"
   "for(int i=0;i<4;i++){s[i]=(unsigned)a+i;d[i]=(unsigned)a+i;}d[2]=0xFFFFFFFFu;"
   "unsigned int*ps=s,*pd=d;unsigned long n=4;unsigned char e=0,b=0;"
   "__asm__ volatile(\"cld; repe cmpsl; sete %0; setb %1\""
   ":\"=&q\"(e),\"=&q\"(b),\"+S\"(ps),\"+D\"(pd),\"+c\"(n)::\"memory\",\"cc\");"
   "return (long)e+(long)b*2+(long)((char*)ps-(char*)s)*4+(long)n*1024;}\n",
   {0x100}, "RepCmpScas", 0},

  {"repe_cmpsw",
   "long f(long a){unsigned short s[6],d[6];"
   "for(int i=0;i<6;i++){s[i]=(unsigned short)(a+i);d[i]=(unsigned short)(a+i);}"
   "d[4]=0x7FFF;"
   "unsigned short*ps=s,*pd=d;unsigned long n=6;unsigned char e=0,b=0;"
   "__asm__ volatile(\"cld; repe cmpsw; sete %0; setb %1\""
   ":\"=&q\"(e),\"=&q\"(b),\"+S\"(ps),\"+D\"(pd),\"+c\"(n)::\"memory\",\"cc\");"
   "return (long)e+(long)b*2+(long)((char*)ps-(char*)s)*4+(long)n*1024;}\n",
   {0x2000}, "RepCmpScas", 0},

  {"repe_cmpsq",
   "long f(long a){unsigned long s[3],d[3];"
   "for(int i=0;i<3;i++){s[i]=(unsigned long)a+i;d[i]=(unsigned long)a+i;}"
   "d[1]=0;"
   "unsigned long*ps=s,*pd=d;unsigned long n=3;unsigned char e=0,b=0;"
   "__asm__ volatile(\"cld; repe cmpsq; sete %0; setb %1\""
   ":\"=&q\"(e),\"=&q\"(b),\"+S\"(ps),\"+D\"(pd),\"+c\"(n)::\"memory\",\"cc\");"
   "return (long)e+(long)b*2+(long)((char*)ps-(char*)s)*4+(long)n*1024;}\n",
   {0x1234}, "RepCmpScas", 0},

  // repne cmpsb: the inverted termination condition (stop on the first MATCH)
  // must not be lowered as a `repe`.
  {"repne_cmpsb",
   "long f(long a){unsigned char s[8],d[8];"
   "for(int i=0;i<8;i++){s[i]=(unsigned char)(a+i);d[i]=(unsigned char)(a+i+7);}"
   "d[5]=s[5];"
   "unsigned char*ps=s,*pd=d;unsigned long n=8;unsigned char e=0;"
   "__asm__ volatile(\"cld; repne cmpsb; sete %0\""
   ":\"=&q\"(e),\"+S\"(ps),\"+D\"(pd),\"+c\"(n)::\"memory\",\"cc\");"
   "return (long)e+(long)(ps-s)*2+(long)(pd-d)*32+(long)n*512;}\n",
   {0x41}, "RepCmpScas", 0},

  // The `strlen` idiom: repne scasb with AL=0 walks to the terminator, and the
  // length falls out of the (negated) leftover count.
  {"repne_scasb_strlen",
   "long f(long a){unsigned char b[16];for(int i=0;i<16;i++)b[i]=(unsigned char)(a+i);"
   "b[9]=0;unsigned char*p=b;unsigned long n=~0UL;unsigned char e=0;"
   "__asm__ volatile(\"cld; repne scasb; sete %0\""
   ":\"=&q\"(e),\"+D\"(p),\"+c\"(n)"
   ":\"a\"((unsigned char)0):\"memory\",\"cc\");"
   "return (long)e+(long)(p-b)*2+(long)(~n)*64;}\n",
   {0x51}, "RepCmpScas", 0},

  // memchr-style hit and miss: the same scan with a bounded count, once with
  // the byte present and once absent (ZF distinguishes them).
  {"repne_scasb_hit",
   "long f(long a){unsigned char b[8];for(int i=0;i<8;i++)b[i]=(unsigned char)(i+1);"
   "b[6]=(unsigned char)a;unsigned char*p=b;unsigned long n=8;unsigned char e=0;"
   "__asm__ volatile(\"cld; repne scasb; sete %0\""
   ":\"=&q\"(e),\"+D\"(p),\"+c\"(n):\"a\"((unsigned char)a):\"memory\",\"cc\");"
   "return (long)e+(long)(p-b)*2+(long)n*64;}\n",
   {0xAA}, "RepCmpScas", 0},

  {"repne_scasb_miss",
   "long f(long a){unsigned char b[8];for(int i=0;i<8;i++)b[i]=(unsigned char)(i+1);"
   "unsigned char*p=b;unsigned long n=8;unsigned char e=0;"
   "__asm__ volatile(\"cld; repne scasb; sete %0\""
   ":\"=&q\"(e),\"+D\"(p),\"+c\"(n):\"a\"((unsigned char)a):\"memory\",\"cc\");"
   "return (long)e+(long)(p-b)*2+(long)n*64;}\n",
   {0xAA}, "RepCmpScas", 0},

  // repe scasb (repeat while the accumulator matches): stops at the first byte
  // that differs, the complementary prefix to the memchr scan above.
  {"repe_scasb",
   "long f(long a){unsigned char b[8];for(int i=0;i<8;i++)b[i]=(unsigned char)a;"
   "b[4]=(unsigned char)(a+1);unsigned char*p=b;unsigned long n=8;"
   "unsigned char e=0,g=0;"
   "__asm__ volatile(\"cld; repe scasb; sete %0; seta %1\""
   ":\"=&q\"(e),\"=&q\"(g),\"+D\"(p),\"+c\"(n):\"a\"((unsigned char)a)"
   ":\"memory\",\"cc\");"
   "return (long)e+(long)g*2+(long)(p-b)*4+(long)n*128;}\n",
   {0x33}, "RepCmpScas", 0},

  // Wider scans: scasl/scasq compare the full accumulator width and stride by
  // it, so a byte-granular model would stop in the wrong place.
  {"repne_scasl",
   "long f(long a){unsigned int b[6];for(int i=0;i<6;i++)b[i]=(unsigned)i+1;"
   "b[3]=(unsigned)a;unsigned int*p=b;unsigned long n=6;unsigned char e=0;"
   "__asm__ volatile(\"cld; repne scasl; sete %0\""
   ":\"=&q\"(e),\"+D\"(p),\"+c\"(n):\"a\"((unsigned)a):\"memory\",\"cc\");"
   "return (long)e+(long)((char*)p-(char*)b)*2+(long)n*256;}\n",
   {0xDEAD}, "RepCmpScas", 0},

  {"repne_scasq",
   "long f(long a){unsigned long b[4];for(int i=0;i<4;i++)b[i]=(unsigned long)i+1;"
   "b[2]=(unsigned long)a;unsigned long*p=b;unsigned long n=4;unsigned char e=0;"
   "__asm__ volatile(\"cld; repne scasq; sete %0\""
   ":\"=&q\"(e),\"+D\"(p),\"+c\"(n):\"a\"((unsigned long)a):\"memory\",\"cc\");"
   "return (long)e+(long)((char*)p-(char*)b)*2+(long)n*256;}\n",
   {0xC0FFEEUL}, "RepCmpScas", 0},

  // Backward (DF=1) scan: `std` must reach the emitted loop, and the flags must
  // come from the element BELOW the final pointer, not above it.
  {"repe_cmpsb_backward",
   "long f(long a){unsigned char s[8],d[8];"
   "for(int i=0;i<8;i++){s[i]=(unsigned char)(a+i);d[i]=(unsigned char)(a+i);}"
   "d[5]=0x01;"
   "unsigned char*ps=s+7,*pd=d+7;unsigned long n=8;unsigned char e=0,b=0;"
   "__asm__ volatile(\"std; repe cmpsb; cld; sete %0; setb %1\""
   ":\"=&q\"(e),\"=&q\"(b),\"+S\"(ps),\"+D\"(pd),\"+c\"(n)::\"memory\",\"cc\");"
   "return (long)e+(long)b*2+(long)(ps-s)*4+(long)n*1024;}\n",
   {0x61}, "RepCmpScas", 0},

  // A zero entry count must leave every register (and the memory) untouched:
  // the loop body never runs, so RSI/RDI/RCX come straight back out.
  {"repe_cmpsb_zero_count",
   "long f(long a){unsigned char s[4]={1,2,3,4},d[4]={9,9,9,9};"
   "unsigned char*ps=s,*pd=d;unsigned long n=0;"
   "__asm__ volatile(\"cld; repe cmpsb\""
   ":\"+S\"(ps),\"+D\"(pd),\"+c\"(n)::\"memory\",\"cc\");"
   "return (long)(ps-s)+(long)(pd-d)*4+(long)n*16+(long)a;}\n",
   {0x7}, "RepCmpScas", 0},

  // With RCX==0 the hardware never dereferences RSI/RDI.  Keep the pointers
  // deliberately invalid so an unconditional flag-reconstruction load cannot
  // hide behind otherwise-valid test buffers.
  {"repe_cmpsb_zero_count_invalid_ptrs",
   "long f(long a){unsigned char*ps=(unsigned char*)1,*pd=(unsigned char*)2;"
   "unsigned long n=(unsigned long)a;unsigned char e=0;"
   "__asm__ volatile(\"cmpq %4,%4; cld; repe cmpsb; sete %0\""
   ":\"=&q\"(e),\"+S\"(ps),\"+D\"(pd),\"+c\"(n):\"r\"(a):\"memory\",\"cc\");"
   "return (long)e+(long)ps*2+(long)pd*8+(long)n*32;}\n",
   {0}, "RepCmpScas", 0},

  {"repne_scasb_zero_count",
   "long f(long a){unsigned char b[4]={1,2,3,4};unsigned char*p=b;unsigned long n=0;"
   "__asm__ volatile(\"cld; repne scasb\""
   ":\"+D\"(p),\"+c\"(n):\"a\"((unsigned char)a):\"memory\",\"cc\");"
   "return (long)(p-b)+(long)n*16+(long)a;}\n",
   {0x9}, "RepCmpScas", 0},

  // The accumulator must survive a scan: SCAS reads AL but never writes it, so
  // the old placeholder's RAX clobber showed up as a lost live value here.
  {"repne_scasb_preserves_rax",
   "long f(long a){unsigned char b[8];for(int i=0;i<8;i++)b[i]=(unsigned char)(i+1);"
   "b[5]=(unsigned char)a;unsigned char*p=b;unsigned long n=8;unsigned long rax;"
   "__asm__ volatile(\"cld; repne scasb; movq %%rax,%0\""
   ":\"=r\"(rax),\"+D\"(p),\"+c\"(n):\"a\"((unsigned long)(unsigned char)a)"
   ":\"memory\",\"cc\");"
   "return (long)rax+(long)(p-b)*256+(long)n*65536;}\n",
   {0x5C}, "RepCmpScas", 0},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(RepCmpScas, X64RepCmpScasRT, ::testing::ValuesIn(kX64),
                         rtTCName);

// ============================================================================
// i386: the same loop through the 32-bit ESI/EDI/ECX views.  Issue #10's report
// was an x86 binary, and the lowering picks its register width from the target
// pointer size, so the narrow path needs its own coverage.
// ============================================================================
class X86RepCmpScasRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86RepCmpScasRT, Verify) { roundTripX86(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX86 = {
  {"x86_repe_cmpsb",
   "int x86_repe_cmpsb(int a){unsigned char s[8],d[8];"
   "for(int i=0;i<8;i++){s[i]=(unsigned char)(a+i);d[i]=(unsigned char)(a+i);}"
   "d[3]=0x80;"
   "unsigned char*ps=s,*pd=d;unsigned int n=8;unsigned char e=0,b=0;"
   "__asm__ volatile(\"cld; repe cmpsb; sete %0; setb %1\""
   ":\"=&q\"(e),\"=&q\"(b),\"+S\"(ps),\"+D\"(pd),\"+c\"(n)::\"memory\",\"cc\");"
   "return (int)e+(int)b*2+(int)(ps-s)*4+(int)(pd-d)*64+(int)n*1024;}\n",
   {0x21}, "RepCmpScas", 0},

  {"x86_repne_scasb_strlen",
   "int x86_repne_scasb_strlen(int a){unsigned char b[16];"
   "for(int i=0;i<16;i++)b[i]=(unsigned char)(a+i);b[6]=0;"
   "unsigned char*p=b;unsigned int n=0xFFFFFFFFu;unsigned char e=0;"
   "__asm__ volatile(\"cld; repne scasb; sete %0\""
   ":\"=&q\"(e),\"+D\"(p),\"+c\"(n):\"a\"((unsigned char)0)"
   ":\"memory\",\"cc\");"
   "return (int)e+(int)(p-b)*2+(int)(~n)*64;}\n",
   {0x51}, "RepCmpScas", 0},

  {"x86_repe_cmpsl",
   "int x86_repe_cmpsl(int a){unsigned int s[4],d[4];"
   "for(int i=0;i<4;i++){s[i]=(unsigned)a+i;d[i]=(unsigned)a+i;}d[2]=0xFFFFFFFFu;"
   "unsigned int*ps=s,*pd=d;unsigned int n=4;unsigned char e=0,b=0;"
   "__asm__ volatile(\"cld; repe cmpsl; sete %0; setb %1\""
   ":\"=&q\"(e),\"=&q\"(b),\"+S\"(ps),\"+D\"(pd),\"+c\"(n)::\"memory\",\"cc\");"
   "return (int)e+(int)b*2+(int)((char*)ps-(char*)s)*4+(int)n*1024;}\n",
   {0x100}, "RepCmpScas", 0},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(RepCmpScas, X86RepCmpScasRT, ::testing::ValuesIn(kX86),
                         rtTCName);
