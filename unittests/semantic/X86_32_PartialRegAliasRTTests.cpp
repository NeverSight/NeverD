//===- X86_32_PartialRegAliasRTTests.cpp - i386 partial-reg alias -*-C++-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// The i386 dual of X86_PartialRegAliasRTTests.  The x86-64 sub-register alias
// class (write a wide reg, read AL/AH/AX, optimizer folds the narrow read to 0
// or mis-merges) was probed only on x86-64; the i386 path through the same
// LowToMed SUBBYTES/merge machinery has a 4-byte parent (EAX is the full reg,
// AX/AL/AH are its partial writes) and the less-mature i386 lift/ABI added in
// #375.  These inline-asm chains exercise partial writes/reads clang never
// emits from plain C, NeverD optimizer ON, original (native) vs lifted:
//   - 16-bit AX write merges low 16, PRESERVES upper 16 of EAX
//   - 8-bit AL/AH writes at offset 0 / offset 1 preserve the rest
//   - movzx/movsx from a sub-register after a wide write
//   - AH (non-zero byte offset) arithmetic and multi-hop alias chains
// EBX is avoided (i386 PIC base register); the second GPR uses ECX/EDX.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X86PartialAliasRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86PartialAliasRT, Verify) { roundTripX86(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX86 = {
  // 16-bit AX write must MERGE (preserve upper 16 bits of EAX).
  {"ax_merge",
   "unsigned f(unsigned a){ unsigned r;\n"
   "  __asm__ volatile(\"movl %1,%%eax\\n\\tmovw $0x3344,%%ax\\n\\tmovl %%eax,%0\"\n"
   "    :\"=r\"(r):\"r\"(a):\"eax\");\n"
   "  return r; }\n",
   {0xAABB1122ULL}, "PartialAlias32"},

  // 8-bit AL write merges low byte, preserves the rest.
  {"al_merge",
   "unsigned f(unsigned a){ unsigned r;\n"
   "  __asm__ volatile(\"movl %1,%%eax\\n\\tmovb $0x99,%%al\\n\\tmovl %%eax,%0\"\n"
   "    :\"=r\"(r):\"r\"(a):\"eax\");\n"
   "  return r; }\n",
   {0xAABBCCDDULL}, "PartialAlias32"},

  // 8-bit AH write touches bits [15:8] only (non-zero byte offset).
  {"ah_merge",
   "unsigned f(unsigned a){ unsigned r;\n"
   "  __asm__ volatile(\"movl %1,%%eax\\n\\tmovb $0x77,%%ah\\n\\tmovl %%eax,%0\"\n"
   "    :\"=r\"(r):\"r\"(a):\"eax\");\n"
   "  return r; }\n",
   {0xAABBCCDDULL}, "PartialAlias32"},

  // MOVZX from AL after a wide write (the historical fold-to-0 bug, i386).
  {"movzx_al",
   "unsigned f(unsigned a){ unsigned r;\n"
   "  __asm__ volatile(\"movl %1,%%eax\\n\\tmovzbl %%al,%%ecx\\n\\tmovl %%ecx,%0\"\n"
   "    :\"=r\"(r):\"r\"(a):\"eax\",\"ecx\");\n"
   "  return r; }\n",
   {0x11227788ULL}, "PartialAlias32"},

  // MOVSX from AX after a wide write (sign-extend the low 16).
  {"movsx_ax",
   "unsigned f(unsigned a){ unsigned r;\n"
   "  __asm__ volatile(\"movl %1,%%eax\\n\\tmovswl %%ax,%%ecx\\n\\tmovl %%ecx,%0\"\n"
   "    :\"=r\"(r):\"r\"(a):\"eax\",\"ecx\");\n"
   "  return r; }\n",
   {0x112280F0ULL}, "PartialAlias32"},

  // AH arithmetic then read full EAX (offset-1 byte read/modify/write).
  {"ah_arith",
   "unsigned f(unsigned a){ unsigned r;\n"
   "  __asm__ volatile(\"movl %1,%%eax\\n\\taddb $0x40,%%ah\\n\\txorb %%al,%%ah\\n\\tmovl %%eax,%0\"\n"
   "    :\"=r\"(r):\"r\"(a):\"eax\",\"cc\");\n"
   "  return r; }\n",
   {0x12345678ULL}, "PartialAlias32"},

  // Multi-hop: AX merge -> AL write -> AH write -> movzwl -> wide add.
  {"multihop",
   "unsigned f(unsigned a){ unsigned r;\n"
   "  __asm__ volatile(\"movl %1,%%eax\\n\\tmovw $0xBEEF,%%ax\\n\\tmovb $0x12,%%al\\n\\tmovb $0x34,%%ah\\n\\t"
   "movzwl %%ax,%%ecx\\n\\taddl %%ecx,%%eax\\n\\tmovl %%eax,%0\"\n"
   "    :\"=r\"(r):\"r\"(a):\"eax\",\"ecx\");\n"
   "  return r; }\n",
   {0x76543210ULL}, "PartialAlias32"},

  // Two distinct sub-registers (AL and CL) interacting through a wide op.
  {"al_cl_cross",
   "unsigned f(unsigned a){ unsigned r;\n"
   "  __asm__ volatile(\"movl %1,%%eax\\n\\tmovl %1,%%ecx\\n\\tmovb $0x10,%%al\\n\\tmovb $0x20,%%cl\\n\\t"
   "addl %%ecx,%%eax\\n\\tmovl %%eax,%0\"\n"
   "    :\"=r\"(r):\"r\"(a):\"eax\",\"ecx\",\"cc\");\n"
   "  return r; }\n",
   {0x05060708ULL}, "PartialAlias32"},

  // 16-bit add into AX with carry out, then read full EAX (AX wraps, upper 16
  // preserved, carry stays in flags only).
  {"ax_add_wrap",
   "unsigned f(unsigned a){ unsigned r;\n"
   "  __asm__ volatile(\"movl %1,%%eax\\n\\tmovw $0xFFF0,%%ax\\n\\taddw $0x0020,%%ax\\n\\tmovl %%eax,%0\"\n"
   "    :\"=r\"(r):\"r\"(a):\"eax\",\"cc\");\n"
   "  return r; }\n",
   {0x55443322ULL}, "PartialAlias32"},

  // AL set, then AH set, then read AX (combine two independent byte writes into
  // a 16-bit read) and add into the preserved upper 16.
  {"al_ah_to_ax",
   "unsigned f(unsigned a){ unsigned r;\n"
   "  __asm__ volatile(\"movl %1,%%eax\\n\\tmovb $0xAB,%%al\\n\\tmovb $0xCD,%%ah\\n\\t"
   "movzwl %%ax,%%ecx\\n\\taddl %%ecx,%%eax\\n\\tmovl %%eax,%0\"\n"
   "    :\"=r\"(r):\"r\"(a):\"eax\",\"ecx\");\n"
   "  return r; }\n",
   {0x10203040ULL}, "PartialAlias32"},

  // 16-bit rotate of AX preserves upper 16 of EAX.
  {"ax_rol",
   "unsigned f(unsigned a){ unsigned r;\n"
   "  __asm__ volatile(\"movl %1,%%eax\\n\\trolw $5,%%ax\\n\\tmovl %%eax,%0\"\n"
   "    :\"=r\"(r):\"r\"(a):\"eax\",\"cc\");\n"
   "  return r; }\n",
   {0x1234ABCDULL}, "PartialAlias32"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(PartialAlias32, X86PartialAliasRT,
                         ::testing::ValuesIn(kX86), rtTCName);
