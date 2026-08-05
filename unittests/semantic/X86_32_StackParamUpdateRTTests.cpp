//===- X86_32_StackParamUpdateRTTests.cpp - i386 mutable stack param -C++-===//
//
// Hardening probes for the i386 cdecl mutable-parameter-home fix (#493): a
// parameter passed on the stack ([ebp+8]...) that is updated IN PLACE and read
// back must observe the write.  detectCdeclStackParams previously folded every
// home-slot load to COPY Param (the original incoming value), dropping the
// loop-carried update; the fix keeps a written slot as memory and spills the
// argument into its home at entry.  These exercise the corners the fix must get
// right: a wide (two-slot) parameter, two parameters updated at once, a mixed
// read-only + written pair, and a written parameter forwarded into a call.  All
// fold to one integer result, NeverD optimizer ON, native i386 vs lifted.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X86StackParamUpdateRT : public SemanticRoundTripFixture,
                              public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86StackParamUpdateRT, Verify) { roundTripX86(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX86 = {
  // The base case: a single int parameter updated each iteration, read back.
  {"acc",
   "unsigned f(unsigned x){ unsigned r=0;\n"
   "  for(int i=0;i<50;i++){ x = x*1103515245u + (unsigned)i; r ^= x>>(i&15); }\n"
   "  return r; }\n",
   {0x9E3779B9u}, "StackParamUpd32", 0},

  // A wide (8-byte, two-slot) parameter updated in place: both home slots must be
  // seeded and the store must cover both.  Passed as two 32-bit cdecl slots
  // (low, high); the 64-bit multiply is inlined on i386 (no libcall).
  {"wide",
   "unsigned long long f(unsigned long long x){ unsigned long long r=0;\n"
   "  for(int i=0;i<50;i++){ x = x*6364136223846793005ULL + (unsigned)i;\n"
   "    r += x>>32; r ^= x; }\n"
   "  return r; }\n",
   {0x89ABCDEFu, 0x01234567u}, "StackParamUpd32", 0},

  // Two parameters both updated each iteration (slots 0 and 1 both written).
  {"two",
   "unsigned f(unsigned a, unsigned b){ unsigned r=0;\n"
   "  for(int i=0;i<40;i++){ a=a*48271u+1u; b=b*16807u+3u; r ^= (a+b)>>(i&7); }\n"
   "  return r; }\n",
   {0x12345678u, 0x9ABCDEF0u}, "StackParamUpd32", 0},

  // Mixed: one parameter updated (slot 0 -> memory), one read-only (slot 1 keeps
  // the COPY-to-Param optimization).  Both must read the right value.
  {"mixed",
   "unsigned f(unsigned k, unsigned n){ unsigned r=0;\n"
   "  for(unsigned i=0;i<40u;i++){ k = k*1103515245u + n; r ^= k>>(i&7); }\n"
   "  return r + n; }\n",
   {0xCAFEBABEu, 0x0000B711u}, "StackParamUpd32", 0},

  // A written parameter forwarded into a call each iteration: the call argument
  // must read the updated home, not the original incoming value.
  {"callupd",
   "static unsigned __attribute__((noinline)) g(unsigned v){ return v*2654435761u + 17u; }\n"
   "unsigned f(unsigned x){ unsigned r=0;\n"
   "  for(int i=0;i<30;i++){ x = x*1103515245u + 12345u; r += g(x); }\n"
   "  return r; }\n",
   {0x0F1E2D3Cu}, "StackParamUpd32", 0},

  // The parameter's ADDRESS escapes to a noinline callee that WRITES it: the
  // write happens through the escaped pointer (no in-function store), so the
  // home slot must still be backed by memory or the read returns the original.
  {"escwr",
   "static void __attribute__((noinline)) addto(unsigned *p, unsigned d){ *p += d; }\n"
   "unsigned f(unsigned x){ addto(&x, 0x1111u); addto(&x, x>>3); return x; }\n",
   {0x40000000u}, "StackParamUpd32", 0},

  // Address escapes into a loop of read-modify-writes through the pointer.
  {"escloop",
   "static void __attribute__((noinline)) step(unsigned *p){ *p = *p*1103515245u + 12345u; }\n"
   "unsigned f(unsigned x){ for(int i=0;i<20;i++) step(&x); return x; }\n",
   {0x12345678u}, "StackParamUpd32", 0},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(StackParamUpd32, X86StackParamUpdateRT,
                         ::testing::ValuesIn(kX86), rtTCName);
