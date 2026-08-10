//===- ARM32_ShiftedIndexRTTests.cpp - ROR/RRX shifted operands ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for ARM32 ROR/RRX barrel-shift handling that the lifter got
// wrong:
//
//  (1) Register-offset memory addressing `ldr/str Rt, [Rn, Rm, ror #k]` —
//      the addressing paths (operandEffAddr + the hand-rolled LDR/STR EA
//      builders) only recognised LSL/LSR/ASR and mapped everything else (ROR)
//      to a plain INT_LEFT, so a rotated index was scaled by a left shift
//      instead of rotated (wrong address).
//
//  (2) RRX (rotate right through carry by one) in shifted data-processing
//      operands (`mov rd, rm, rrx`) and in addressing — silently dropped
//      (capstone reports a zero shift amount, so the shared ApplyShift bailed
//      out and the addressing builders left-shifted by zero / passed through).
//
// The data-processing ROR path was already correct (rotate idiom); the bug was
// confined to the addressing builders (ROR) and to RRX everywhere.  Prior probes
// only used LSL/LSR/ASR index shifts and never RRX, masking both.
//
// Memory probes use a DATA-region pointer arg (0x500000) initialised through the
// pointer, not a stack array, so the load/store data is plain memory the backend
// forwards correctly — independent of the ARM32 stack-slot model.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32ShiftedIndexRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32ShiftedIndexRT, Verify) { roundTripARM32(GetParam()); }

// Fields: Category, OptLevel, ExtraFlags, NoOpt, ClangTargetOverride, UcCpuModel
#define ARMSI "ShiftedIndex", 1, "", false, "", -1

// clang-format off

static const std::vector<RoundTripTC> kARMShiftedIndex = {

  // --- RED: ROR register-offset addressing (was lifted as a left shift) ---

  // ldr [p, idx, ror #2]; idx=0x10 -> ror=4 bytes -> p[1]=0x22222222.
  // Buggy (INT_LEFT): 0x10<<2 = 64 bytes -> p[16] (zeroed DATA) = 0.
  {"ldr_ror2",
   "unsigned ldr_ror2(unsigned a, unsigned *p){"
   " p[0]=0x11111111u;p[1]=0x22222222u;p[2]=0x33333333u;p[3]=0x44444444u;"
   " unsigned r;"
   " __asm__ volatile(\"ldr %0,[%1,%2,ror #2]\":\"=r\"(r):\"r\"(p),\"r\"(a):\"memory\");"
   " return r; }\n",
   {0x10, 0x500000}, ARMSI},

  // ldr [p, idx, ror #4]; idx=0x80 -> ror=8 bytes -> p[2]=0x33333333.
  // Buggy: 0x80<<4 = 2048 bytes -> p[512] (zeroed DATA) = 0.
  {"ldr_ror4",
   "unsigned ldr_ror4(unsigned a, unsigned *p){"
   " p[0]=0x11111111u;p[1]=0x22222222u;p[2]=0x33333333u;p[3]=0x44444444u;"
   " unsigned r;"
   " __asm__ volatile(\"ldr %0,[%1,%2,ror #4]\":\"=r\"(r):\"r\"(p),\"r\"(a):\"memory\");"
   " return r; }\n",
   {0x80, 0x500000}, ARMSI},

  // str [p, idx, ror #2]; idx=0x10 -> stores 0xCAFEBABE into p[1], read back.
  // Buggy: stores at p[16]; p[1] stays 0.
  {"str_ror2",
   "unsigned str_ror2(unsigned a, unsigned *p){"
   " p[0]=0;p[1]=0;p[2]=0;p[3]=0; unsigned v=0xCAFEBABEu;"
   " __asm__ volatile(\"str %0,[%1,%2,ror #2]\"::\"r\"(v),\"r\"(p),\"r\"(a):\"memory\");"
   " return p[1]; }\n",
   {0x10, 0x500000}, ARMSI},

  // --- RED: RRX (rotate right through carry) in a data-processing operand ---

  // carry=1 (cmp r12,r12 -> no borrow -> C=1): rrx(a)=(a>>1)|(1<<31).
  // a=2 -> 0x80000001.  Buggy (RRX dropped): mov r0,r1 = a = 2.
  {"mov_rrx_c1",
   "unsigned mov_rrx_c1(unsigned a){"
   " unsigned r;"
   " __asm__ volatile(\"mov r12,#0\\n\\tcmp r12,r12\\n\\tmov %0,%1,rrx\""
   ":\"=r\"(r):\"r\"(a):\"r12\",\"cc\");"
   " return r; }\n",
   {2}, ARMSI},

  // carry=0 (cmp #0,#1 -> borrow -> C=0): rrx(a)=a>>1.  a=4 -> 2.
  // Buggy (RRX dropped): a = 4.
  {"mov_rrx_c0",
   "unsigned mov_rrx_c0(unsigned a){"
   " unsigned r;"
   " __asm__ volatile(\"mov r12,#0\\n\\tcmp r12,#1\\n\\tmov %0,%1,rrx\""
   ":\"=r\"(r):\"r\"(a):\"r12\",\"cc\");"
   " return r; }\n",
   {4}, ARMSI},

  // RRX register-offset addressing (carry=0): rrx(idx)=idx>>1.  idx=8 -> 4
  // bytes -> p[1]=0x22222222.  Buggy (old guard skipped value==0): idx
  // unchanged = 8 bytes -> p[2]=0x33333333.
  {"ldr_rrx",
   "unsigned ldr_rrx(unsigned a, unsigned *p){"
   " p[0]=0x11111111u;p[1]=0x22222222u;p[2]=0x33333333u;p[3]=0x44444444u;"
   " unsigned r;"
   " __asm__ volatile(\"mov r12,#0\\n\\tcmp r12,#1\\n\\tldr %0,[%1,%2,rrx]\""
   ":\"=r\"(r):\"r\"(p),\"r\"(a):\"r12\",\"cc\",\"memory\");"
   " return r; }\n",
   {8, 0x500000}, ARMSI},

  // --- Controls: should pass with or without the fix ---

  // Data-processing ROR was already correct (rotate); guard against regression.
  // ror(0x12, 4) = (0x12>>4)|(0x12<<28) = 1 | 0x20000000 = 0x20000001.
  {"add_ror_ctl",
   "unsigned add_ror_ctl(unsigned a){"
   " unsigned r;"
   " __asm__ volatile(\"add %0,%2,%1,ror #4\":\"=r\"(r):\"r\"(a),\"r\"(0):\"cc\");"
   " return r; }\n",
   {0x12}, ARMSI},

  // LSL index addressing (the path that already worked).  idx=3 -> 12 bytes
  // -> p[3]=0x44444444.
  {"ldr_lsl_ctl",
   "unsigned ldr_lsl_ctl(unsigned a, unsigned *p){"
   " p[0]=0x11111111u;p[1]=0x22222222u;p[2]=0x33333333u;p[3]=0x44444444u;"
   " unsigned r;"
   " __asm__ volatile(\"ldr %0,[%1,%2,lsl #2]\":\"=r\"(r):\"r\"(p),\"r\"(a):\"memory\");"
   " return r; }\n",
   {3, 0x500000}, ARMSI},

  // LSR index addressing control.  idx=0x10 -> 4 bytes -> p[1]=0x22222222.
  {"ldr_lsr_ctl",
   "unsigned ldr_lsr_ctl(unsigned a, unsigned *p){"
   " p[0]=0x11111111u;p[1]=0x22222222u;p[2]=0x33333333u;p[3]=0x44444444u;"
   " unsigned r;"
   " __asm__ volatile(\"ldr %0,[%1,%2,lsr #2]\":\"=r\"(r):\"r\"(p),\"r\"(a):\"memory\");"
   " return r; }\n",
   {0x10, 0x500000}, ARMSI},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(ShiftedIndex, ARM32ShiftedIndexRT,
                         ::testing::ValuesIn(kARMShiftedIndex), rtTCName);
