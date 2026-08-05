//===- X64_VexVectorStoreRTTests.cpp - VEX VMOV* store-to-mem RT -*- C++ -*-===//
//
// VEX-encoded vector/scalar moves with a MEMORY DESTINATION
// (`vmovdqu/vmovups/vmovaps/vmovsd/vmovss/vmovq/vmovd  [mem], xmm`) lost their
// store entirely: the VEX `VMOV*` lift handler used
//
//     NdVar Dst = operandWrite(operands[0]);
//     S.emit(COPY, Dst, Src);
//
// with no `operands[0].type == X86_OP_MEM` branch.  operandWrite() on a memory
// operand yields a DISCARDED ram(0) placeholder (it does not emit a store), so
// `COPY placeholder, Src` is dead and the value never reaches memory — the load
// that reads it back sees stale/uninitialized bytes.
//
// This is the SAME systematic trap fixed for the SSE MOV* path and, in turn, for
// the x86 atomics (CMPXCHG/XADD/XCHG), SIMD lane extract (PEXTR*/EXTRACTPS,
// #339), partial 64-bit moves (MOVHPS/MOVLPS, #340), double-shift / rotate
// memory destinations (SHLD/RCR, #343) and VEXTRACTF128 — but the most basic and
// common VEX vector STORE was never given the `MemDst ? storeToMem : operandWrite`
// guard.  Fix: in the VEX VMOV* handler, when the destination is memory emit an
// explicit storeToMem (truncating to the scalar element width for the
// VMOVSS/VMOVD = 4-byte and VMOVSD/VMOVQ = 8-byte forms); the register-destination
// path is unchanged.
//
// Each probe loads the args into an xmm with a VEX *load* (which already worked)
// then writes them back out through the VEX *store* form under test, and folds
// the read-back memory into the return.  The output buffer is pre-seeded with a
// SENTINEL: if the store is dropped the lifted function returns the sentinel
// while the original returns the real data — a guaranteed mismatch (RED 6/6
// before the fix, GREEN after).  Built `-mavx2` to pin VEX.128 encodings, which
// the bundled Unicorn decodes (only 256-bit ymm VEX forms are undecodable).
// Oracle compares original-Unicorn vs lifted-Unicorn.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VexVectorStoreRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VexVectorStoreRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // ===== vmovdqu (packed 128-bit integer store). =====
  {"vmovdqu_store",
   "unsigned long f(unsigned long a, unsigned long b){\n"
   "  unsigned long in[2]={a,b};\n"
   "  unsigned long out[2]={0xA5A5A5A5A5A5A5A5ul,0x5A5A5A5A5A5A5A5Aul};\n"
   "  __asm__ volatile(\"vmovdqu %1,%%xmm6\\n\\tvmovdqu %%xmm6,%0\"\n"
   "                   :\"=m\"(out):\"m\"(in):\"xmm6\",\"memory\");\n"
   "  return out[0]*1000003ul + out[1]*99ul;\n"
   "}\n",
   {0x1122334455667788ULL, 0x99AABBCCDDEEFF01ULL}, "VexVectorStore", 1, "-mavx2"},

  // ===== vmovups (packed 128-bit FP store — same handler, different opcode). ====
  {"vmovups_store",
   "unsigned long f(unsigned long a, unsigned long b){\n"
   "  unsigned long in[2]={a,b};\n"
   "  unsigned long out[2]={0xDEADBEEFDEADBEEFul,0xFEEDFACEFEEDFACEul};\n"
   "  __asm__ volatile(\"vmovups %1,%%xmm7\\n\\tvmovups %%xmm7,%0\"\n"
   "                   :\"=m\"(out):\"m\"(in):\"xmm7\",\"memory\");\n"
   "  return out[0]*1000003ul + out[1]*99ul;\n"
   "}\n",
   {0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL}, "VexVectorStore", 1, "-mavx2"},

  // ===== vmovsd (scalar 8-byte store — truncates xmm to low qword). =====
  {"vmovsd_store",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long in=a, out=0xC3C3C3C3C3C3C3C3ul;\n"
   "  __asm__ volatile(\"vmovsd %1,%%xmm5\\n\\tvmovsd %%xmm5,%0\"\n"
   "                   :\"=m\"(out):\"m\"(in):\"xmm5\",\"memory\");\n"
   "  return out;\n"
   "}\n",
   {0x1122334455667788ULL}, "VexVectorStore", 1, "-mavx2"},

  // ===== vmovss (scalar 4-byte store — truncates xmm to low dword). =====
  {"vmovss_store",
   "unsigned long f(unsigned long a){\n"
   "  unsigned in=(unsigned)a, out=0xC3C3C3C3u;\n"
   "  __asm__ volatile(\"vmovss %1,%%xmm5\\n\\tvmovss %%xmm5,%0\"\n"
   "                   :\"=m\"(out):\"m\"(in):\"xmm5\",\"memory\");\n"
   "  return (unsigned long)out;\n"
   "}\n",
   {0x00000000DEADBEEFULL}, "VexVectorStore", 1, "-mavx2"},

  // ===== vmovq (8-byte GPR-width store from xmm to memory). =====
  {"vmovq_store",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long in=a, out=0x7777777777777777ul;\n"
   "  __asm__ volatile(\"vmovq %1,%%xmm4\\n\\tvmovq %%xmm4,%0\"\n"
   "                   :\"=m\"(out):\"m\"(in):\"xmm4\",\"memory\");\n"
   "  return out;\n"
   "}\n",
   {0x0BADC0DECAFEF00DULL}, "VexVectorStore", 1, "-mavx2"},

  // ===== vmovd (4-byte store from xmm to memory). =====
  {"vmovd_store",
   "unsigned long f(unsigned long a){\n"
   "  unsigned in=(unsigned)a, out=0x44444444u;\n"
   "  __asm__ volatile(\"vmovd %1,%%xmm4\\n\\tvmovd %%xmm4,%0\"\n"
   "                   :\"=m\"(out):\"m\"(in):\"xmm4\",\"memory\");\n"
   "  return (unsigned long)out;\n"
   "}\n",
   {0x00000000ABCD1234ULL}, "VexVectorStore", 1, "-mavx2"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(VexVectorStore, X64VexVectorStoreRT,
                         ::testing::ValuesIn(kX64), rtTCName);
