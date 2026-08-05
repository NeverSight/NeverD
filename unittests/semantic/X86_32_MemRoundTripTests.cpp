//===- X86_32_MemRoundTripTests.cpp - i386 memory/control roundtrip -*- C++ -*-=//
//
// Second i386 roundtrip batch, building on the cdecl integer coverage: stack
// locals and arrays (negative-esp frame slots), control flow, and functions
// with more than four arguments (deeper [esp+N] offsets exercising the cdecl
// stack-parameter recovery across an adjusted esp).
//
// The loops here carry a dependency so clang does not auto-vectorize them into a
// PC-relative constant-pool load; the i386 PIC get-PC thunk that such loads rely
// on (`call $+5; pop reg`) is tracked separately as a future i386 target.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X86MemRT : public SemanticRoundTripFixture,
                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86MemRT, Verify) { roundTripX86(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX86Mem = {
  // Stack array built with a loop-carried recurrence, then reduced (frame
  // locals at negative esp offsets; the dependency blocks vectorization).
  {"x86_arrchain",
   "int x86_arrchain(int a){ int v[16]; v[0]=a|1;\n"
   "  for(int i=1;i<16;i++) v[i]=v[i-1]*31+i*a;\n"
   "  int s=0; for(int i=0;i<16;i++) s=(s^v[i])+(s<<1); return s; }\n",
   {0x53ULL}, "X86Mem", 2, ""},

  // Iterative Fibonacci with loop-carried locals.
  {"x86_fib",
   "int x86_fib(int a){ int n=(a&31)+2, x=0, y=1; for(int i=0;i<n;i++){ int t=x+y; x=y; y=t; } return x; }\n",
   {0x1FULL}, "X86Mem", 2, ""},

  // Nested loops over a stack matrix (cross-indexed reduce).
  {"x86_matrix",
   "int x86_matrix(int a){ int m[4][4]; for(int i=0;i<4;i++) for(int j=0;j<4;j++) m[i][j]=a*(i+1)+j;\n"
   "  int s=0; for(int i=0;i<4;i++) for(int j=0;j<4;j++) s+=m[i][j]*m[j][i]; return s; }\n",
   {0x11ULL}, "X86Mem", 2, ""},

  // Byte FNV hash over a stack buffer built with a recurrence (no vectorize).
  {"x86_bytehash",
   "int x86_bytehash(int a){ unsigned char b[32]; b[0]=(unsigned char)a;\n"
   "  for(int i=1;i<32;i++) b[i]=(unsigned char)(b[i-1]*5+i+a);\n"
   "  unsigned h=2166136261u; for(int i=0;i<32;i++){ h^=b[i]; h*=16777619u; } return (int)h; }\n",
   {0x7BULL}, "X86Mem", 2, ""},

  // Five-argument cdecl: args at [esp+4..esp+20] survive an adjusted esp.
  {"x86_args5",
   "int x86_args5(int a, int b, int c, int d, int e){ return a*16+b*8+c*4+d*2+e; }\n",
   {1ULL, 2ULL, 3ULL, 4ULL, 5ULL}, "X86Mem", 2, ""},

  // Branch-heavy state machine driven by argument bits.
  {"x86_fsm",
   "int x86_fsm(int a){ int st=0, acc=0;\n"
   "  for(int i=0;i<24;i++){ int sym=(a>>(i&15))&3;\n"
   "    switch(st){ case 0: st=sym?1:2; acc+=sym; break;\n"
   "      case 1: st=(sym==2)?0:1; acc+=sym*3; break;\n"
   "      default: st=(sym==1)?2:0; acc-=sym; } }\n"
   "  return acc; }\n",
   {0x2468ACEULL}, "X86Mem", 2, ""},

  // Pointer walk with a loop-carried accumulator over a recurrence-built array.
  {"x86_ptrwalk",
   "int x86_ptrwalk(int a){ int v[20]; v[0]=a;\n"
   "  for(int i=1;i<20;i++) v[i]=(v[i-1]*1103515245+12345)^(i*a);\n"
   "  int *p=v, s=0; for(int i=0;i<20;i++){ s += (*p>0)?*p:-*p; p++; } return s; }\n",
   {0x39ULL}, "X86Mem", 2, ""},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(X86Mem, X86MemRT, ::testing::ValuesIn(kX86Mem),
                         rtTCName);
