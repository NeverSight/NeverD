//===- X86_PartialRegAliasRTTests.cpp - partial-register alias stress -C++-===//
//
// Adversarial probes for the x86 sub-register aliasing class that bit hard
// historically (write a wide reg, read AL/AH/AX, optimizer folds the narrow
// read to 0 / mis-merges).  These inline-asm sequences chain partial writes
// and reads that clang never emits from plain C, with the NeverD optimizer ON,
// so any wrong SUBBYTES/merge in LowToMed -> MedIR diverges from the original:
//   - 16-bit AX write merges low 16, PRESERVES upper 48 (not zero-extended)
//   - 8-bit AL/AH writes at offset 0 / offset 1 preserve the rest
//   - 32-bit EAX write ZERO-extends (clears upper 32) -- the x86 quirk
//   - movzx/movsx from a sub-register after a wide write
//   - multi-hop alias chains and AH (non-zero byte offset) arithmetic
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64PartialAliasRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64PartialAliasRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // 16-bit AX write must MERGE (preserve upper 48 bits of RAX).
  {"ax_merge",
   "unsigned long f(unsigned long a){ unsigned long r;\n"
   "  __asm__ volatile(\"movq %1,%%rax\\n\\tmovw $0x3344,%%ax\\n\\tmovq %%rax,%0\"\n"
   "    :\"=r\"(r):\"r\"(a):\"rax\");\n"
   "  return r; }\n",
   {0xAABBCCDDEEFF1122ULL}, "PartialAlias"},

  // 8-bit AL write merges low byte, preserves the rest.
  {"al_merge",
   "unsigned long f(unsigned long a){ unsigned long r;\n"
   "  __asm__ volatile(\"movq %1,%%rax\\n\\tmovb $0x99,%%al\\n\\tmovq %%rax,%0\"\n"
   "    :\"=r\"(r):\"r\"(a):\"rax\");\n"
   "  return r; }\n",
   {0xAABBCCDDEEFF1122ULL}, "PartialAlias"},

  // 8-bit AH write touches bits [15:8] only (non-zero byte offset).
  {"ah_merge",
   "unsigned long f(unsigned long a){ unsigned long r;\n"
   "  __asm__ volatile(\"movq %1,%%rax\\n\\tmovb $0x77,%%ah\\n\\tmovq %%rax,%0\"\n"
   "    :\"=r\"(r):\"r\"(a):\"rax\");\n"
   "  return r; }\n",
   {0xAABBCCDDEEFF1122ULL}, "PartialAlias"},

  // 32-bit EAX write ZERO-extends -- upper 32 bits of RAX must clear.
  {"eax_zext",
   "unsigned long f(unsigned long a){ unsigned long r;\n"
   "  __asm__ volatile(\"movq %1,%%rax\\n\\tmovl $0x55667788,%%eax\\n\\tmovq %%rax,%0\"\n"
   "    :\"=r\"(r):\"r\"(a):\"rax\");\n"
   "  return r; }\n",
   {0xAABBCCDD11223344ULL}, "PartialAlias"},

  // MOVZX from AL after a wide write (the historical fold-to-0 bug).
  {"movzx_al",
   "unsigned long f(unsigned long a){ unsigned long r;\n"
   "  __asm__ volatile(\"movq %1,%%rax\\n\\tmovzbq %%al,%%rcx\\n\\tmovq %%rcx,%0\"\n"
   "    :\"=r\"(r):\"r\"(a):\"rax\",\"rcx\");\n"
   "  return r; }\n",
   {0x1122334455667788ULL}, "PartialAlias"},

  // MOVSX from AX after a wide write (sign-extend the low 16).
  {"movsx_ax",
   "unsigned long f(unsigned long a){ unsigned long r;\n"
   "  __asm__ volatile(\"movq %1,%%rax\\n\\tmovswq %%ax,%%rcx\\n\\tmovq %%rcx,%0\"\n"
   "    :\"=r\"(r):\"r\"(a):\"rax\",\"rcx\");\n"
   "  return r; }\n",
   {0x11223344556680F0ULL}, "PartialAlias"},

  // AH arithmetic then read full RAX (offset-1 byte read/modify/write).
  {"ah_arith",
   "unsigned long f(unsigned long a){ unsigned long r;\n"
   "  __asm__ volatile(\"movq %1,%%rax\\n\\taddb $0x40,%%ah\\n\\txorb %%al,%%ah\\n\\tmovq %%rax,%0\"\n"
   "    :\"=r\"(r):\"r\"(a):\"rax\",\"cc\");\n"
   "  return r; }\n",
   {0xDEADBEEF12345678ULL}, "PartialAlias"},

  // Multi-hop: AX merge -> AL write -> AH write -> movzwl -> wide add.
  {"multihop",
   "unsigned long f(unsigned long a){ unsigned long r;\n"
   "  __asm__ volatile(\"movq %1,%%rax\\n\\tmovw $0xBEEF,%%ax\\n\\tmovb $0x12,%%al\\n\\tmovb $0x34,%%ah\\n\\t"
   "movzwl %%ax,%%ecx\\n\\taddq %%rcx,%%rax\\n\\tmovq %%rax,%0\"\n"
   "    :\"=r\"(r):\"r\"(a):\"rax\",\"rcx\");\n"
   "  return r; }\n",
   {0xFEDCBA9876543210ULL}, "PartialAlias"},

  // EAX write (zext) then AX write (merge into the now-zeroed upper-cleared rax).
  {"eax_then_ax",
   "unsigned long f(unsigned long a){ unsigned long r;\n"
   "  __asm__ volatile(\"movq %1,%%rax\\n\\tmovl $0xAABBCCDD,%%eax\\n\\tmovw $0x1234,%%ax\\n\\tmovq %%rax,%0\"\n"
   "    :\"=r\"(r):\"r\"(a):\"rax\");\n"
   "  return r; }\n",
   {0x1111222233334444ULL}, "PartialAlias"},

  // Two distinct sub-registers (AL and BL) interacting through a wide op.
  {"al_bl_cross",
   "unsigned long f(unsigned long a){ unsigned long r;\n"
   "  __asm__ volatile(\"movq %1,%%rax\\n\\tmovq %1,%%rbx\\n\\tmovb $0x10,%%al\\n\\tmovb $0x20,%%bl\\n\\t"
   "addq %%rbx,%%rax\\n\\tmovq %%rax,%0\"\n"
   "    :\"=r\"(r):\"r\"(a):\"rax\",\"rbx\",\"cc\");\n"
   "  return r; }\n",
   {0x0102030405060708ULL}, "PartialAlias"},

  // 16-bit add into AX with carry out, then read full RAX (carry stays in flags
  // only, AX wraps, upper 48 preserved).
  {"ax_add_wrap",
   "unsigned long f(unsigned long a){ unsigned long r;\n"
   "  __asm__ volatile(\"movq %1,%%rax\\n\\tmovw $0xFFF0,%%ax\\n\\taddw $0x0020,%%ax\\n\\tmovq %%rax,%0\"\n"
   "    :\"=r\"(r):\"r\"(a):\"rax\",\"cc\");\n"
   "  return r; }\n",
   {0x9988776655443322ULL}, "PartialAlias"},

  // BSWAP on EAX (32-bit) zero-extends; verify upper 32 clears.
  {"bswap_eax",
   "unsigned long f(unsigned long a){ unsigned long r;\n"
   "  __asm__ volatile(\"movq %1,%%rax\\n\\tbswapl %%eax\\n\\tmovq %%rax,%0\"\n"
   "    :\"=r\"(r):\"r\"(a):\"rax\");\n"
   "  return r; }\n",
   {0xAABBCCDD11223344ULL}, "PartialAlias"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(PartialAlias, X64PartialAliasRT,
                         ::testing::ValuesIn(kX64), rtTCName);
