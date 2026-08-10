//===- X64_AtomicMemStoreRTTests.cpp - CMPXCHG/XADD mem write-back -*- C++ -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// x86 read-modify-write atomics with a MEMORY destination must write the result
// back to memory:
//   CMPXCHG m,r   : if AL/AX/EAX/RAX == m then m = r  (always a store)
//   CMPXCHG8B m64 : if EDX:EAX == m then m = ECX:EBX
//   CMPXCHG16B    : if RDX:RAX == m then m = RCX:RBX
//   XADD m,r      : tmp = m + r; r = m; m = tmp
//
// The lifter wrote the result to `operandWrite()` which, for a memory operand,
// returns a discarded `ram(0)` placeholder instead of issuing a STORE (the
// store-back helper `storeToMem` was never called).  So memory was LEFT
// UNCHANGED: CMPXCHG never committed on a match, CMPXCHG8B/16B never wrote at
// all, and XADD never updated the cell.  Existing tests only checked the
// register side effects (RAX / the source reg), never the memory cell, so this
// had zero coverage.  Probes fold the post-op memory cell into the return; the
// discriminating case for CMPXCHG is the MATCH (a mismatch leaves memory alone
// anyway, masking the dropped store).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64AtomicMemStoreRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64AtomicMemStoreRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {

  // CMPXCHG32 MATCH: EAX(50)==mem(50) -> mem=desired(99) (RED before fix: 50).
  {"cmpxchg32_mem_match",
   "unsigned f(unsigned exp,unsigned des){unsigned mem=50;unsigned char ok;"
   "__asm__ volatile(\"cmpxchg %3,%0\\n\\tsetz %1\""
   ":\"+m\"(mem),\"=q\"(ok),\"+a\"(exp):\"r\"(des):\"cc\");"
   "return mem*7u+exp*13u+(unsigned)ok*1000003u;}\n",
   {50, 99}, "AtomMemStore"},

  // CMPXCHG32 NOMATCH control: mem unchanged (50), EAX=old mem(50).
  {"cmpxchg32_mem_nomatch",
   "unsigned f(unsigned exp,unsigned des){unsigned mem=50;unsigned char ok;"
   "__asm__ volatile(\"cmpxchg %3,%0\\n\\tsetz %1\""
   ":\"+m\"(mem),\"=q\"(ok),\"+a\"(exp):\"r\"(des):\"cc\");"
   "return mem*7u+exp*13u+(unsigned)ok*1000003u;}\n",
   {40, 99}, "AtomMemStore"},

  // CMPXCHG64 MATCH.
  {"cmpxchg64_mem_match",
   "unsigned long f(unsigned long exp,unsigned long des){unsigned long mem=50;unsigned char ok;"
   "__asm__ volatile(\"cmpxchg %3,%0\\n\\tsetz %1\""
   ":\"+m\"(mem),\"=q\"(ok),\"+a\"(exp):\"r\"(des):\"cc\");"
   "return mem*7ul+exp*13ul+(unsigned long)ok*1000003ul;}\n",
   {50, 0x1122334455ULL}, "AtomMemStore"},

  // CMPXCHG16 MATCH (16-bit operand).
  {"cmpxchg16_mem_match",
   "unsigned f(unsigned ein,unsigned din){unsigned short mem=5000;"
   "unsigned short exp=(unsigned short)ein,des=(unsigned short)din;unsigned char ok;"
   "__asm__ volatile(\"cmpxchg %3,%0\\n\\tsetz %1\""
   ":\"+m\"(mem),\"=q\"(ok),\"+a\"(exp):\"r\"(des):\"cc\");"
   "return (unsigned)mem*7u+(unsigned)exp*13u+(unsigned)ok*1000003u;}\n",
   {5000, 9999}, "AtomMemStore"},

  // CMPXCHG8 MATCH (8-bit operand, AL accumulator).
  {"cmpxchg8_mem_match",
   "unsigned f(unsigned ein,unsigned din){unsigned char mem=50;"
   "unsigned char exp=(unsigned char)ein,des=(unsigned char)din;unsigned char ok;"
   "__asm__ volatile(\"cmpxchg %3,%0\\n\\tsetz %1\""
   ":\"+m\"(mem),\"=q\"(ok),\"+a\"(exp):\"q\"(des):\"cc\");"
   "return (unsigned)mem*7u+(unsigned)exp*13u+(unsigned)ok*1000003u;}\n",
   {50, 200}, "AtomMemStore"},

  // XCHG [mem],reg: mem<->reg.  RED before fix: only the register half updated,
  // memory left unchanged.
  {"xchg32_mem",
   "unsigned f(unsigned a){unsigned mem=0x11111111u;unsigned y=a;"
   "__asm__ volatile(\"xchg %1,%0\":\"+m\"(mem),\"+r\"(y)::);"
   "return mem*7u+y*13u;}\n",
   {0xAAAAAAAAu}, "AtomMemStore"},
  {"xchg64_mem",
   "unsigned long f(unsigned long a){unsigned long mem=0x1111111111111111ULL;unsigned long y=a;"
   "__asm__ volatile(\"xchg %1,%0\":\"+m\"(mem),\"+r\"(y)::);"
   "return mem*7ul+y*13ul;}\n",
   {0xAAAAAAAAAAAAAAAAULL}, "AtomMemStore"},
  {"xchg8_mem",
   "unsigned f(unsigned a){unsigned char mem=0x11;unsigned char y=(unsigned char)a;"
   "__asm__ volatile(\"xchg %1,%0\":\"+m\"(mem),\"+q\"(y)::);"
   "return (unsigned)mem*7u+(unsigned)y*13u;}\n",
   {0xAA}, "AtomMemStore"},

  // XADD32 mem: mem += r; r = old mem.
  {"xadd32_mem",
   "unsigned f(unsigned a){unsigned mem=100;unsigned y=a;"
   "__asm__ volatile(\"xadd %1,%0\":\"+m\"(mem),\"+r\"(y)::\"cc\");"
   "return mem*7u+y*13u;}\n",
   {5}, "AtomMemStore"},

  // XADD64 mem.
  {"xadd64_mem",
   "unsigned long f(unsigned long a){unsigned long mem=100;unsigned long y=a;"
   "__asm__ volatile(\"xadd %1,%0\":\"+m\"(mem),\"+r\"(y)::\"cc\");"
   "return mem*7ul+y*13ul;}\n",
   {5}, "AtomMemStore"},

  // XADD8 mem.
  {"xadd8_mem",
   "unsigned f(unsigned a){unsigned char mem=100;unsigned char y=(unsigned char)a;"
   "__asm__ volatile(\"xadd %1,%0\":\"+m\"(mem),\"+q\"(y)::\"cc\");"
   "return (unsigned)mem*7u+(unsigned)y*13u;}\n",
   {5}, "AtomMemStore"},

  // CMPXCHG8B MATCH: EDX:EAX==mem -> mem=ECX:EBX (RED before fix: mem unchanged).
  {"cmpxchg8b_match",
   "unsigned long f(){unsigned long long mem=0x1111111122222222ULL;"
   "unsigned eax=0x22222222u,edx=0x11111111u,ebx=0xBBBBBBBBu,ecx=0xCCCCCCCCu;unsigned char ok;"
   "__asm__ volatile(\"cmpxchg8b %0\\n\\tsetz %1\""
   ":\"+m\"(mem),\"=q\"(ok),\"+a\"(eax),\"+d\"(edx):\"b\"(ebx),\"c\"(ecx):\"cc\");"
   "return (unsigned long)(mem ^ ((unsigned long long)ok<<60));}\n",
   {}, "AtomMemStore"},

  // CMPXCHG8B NOMATCH control: mem unchanged; EDX:EAX = loaded mem.
  {"cmpxchg8b_nomatch",
   "unsigned long f(){unsigned long long mem=0x1111111122222222ULL;"
   "unsigned eax=0xDEADBEEFu,edx=0x11111111u,ebx=0xBBBBBBBBu,ecx=0xCCCCCCCCu;unsigned char ok;"
   "__asm__ volatile(\"cmpxchg8b %0\\n\\tsetz %1\""
   ":\"+m\"(mem),\"=q\"(ok),\"+a\"(eax),\"+d\"(edx):\"b\"(ebx),\"c\"(ecx):\"cc\");"
   "return (unsigned long)(mem + eax + ((unsigned long long)ok<<60));}\n",
   {}, "AtomMemStore"},

  // CMPXCHG16B MATCH: RDX:RAX==mem -> mem=RCX:RBX.
  {"cmpxchg16b_match",
   "unsigned long f(){"
   "struct{unsigned long lo,hi;} mem __attribute__((aligned(16)))={0xAAAA,0xBBBB};"
   "unsigned long rax=0xAAAA,rdx=0xBBBB,rbx=0x1234,rcx=0x5678;unsigned char ok;"
   "__asm__ volatile(\"cmpxchg16b %0\\n\\tsetz %1\""
   ":\"+m\"(mem),\"=q\"(ok),\"+a\"(rax),\"+d\"(rdx):\"b\"(rbx),\"c\"(rcx):\"cc\");"
   "return mem.lo*7ul+mem.hi*13ul+(unsigned long)ok*1000003ul;}\n",
   {}, "AtomMemStore"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(AtomMemStore, X64AtomicMemStoreRT,
                         ::testing::ValuesIn(kX64), rtTCName);
