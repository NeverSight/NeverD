//===- ARM32_AcqRelWidthRTTests.cpp - ARMv8 AArch32 acq/rel width --------===//
//
// Roundtrip probes for ARMv8 AArch32 acquire/release (LDA/STL) and exclusive
// (LDREX) byte/halfword forms.  These share a handler group that issued
// `LOAD Dst, {EA}` / `STORE EA, Src` using the full 4-byte register width —
// so LDAB/LDAH/LDREXB/LDREXH loaded 4 bytes (instead of a zero-extended
// 1/2-byte value) and STLB/STLH stored 4 bytes (instead of the low 1/2 byte).
//
// LDA/STL and a standalone LDREX{B,H} load do not depend on the exclusive
// monitor, so their loaded value / stored bytes roundtrip cleanly.  Probes use
// adjacent non-zero bytes so a wrong width diverges.  Needs an ARMv8 AArch32
// baseline (LDA/STL are ARMv8) + the MAX Unicorn CPU.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32AcqRelWidthRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32AcqRelWidthRT, Verify) { roundTripARM32(GetParam()); }

// Fields: Category, OptLevel, ExtraFlags, NoOpt, ClangTargetOverride, UcCpuModel
#define A32V8 "AcqRelWidth", 1, \
  "-march=armv8-a -mfpu=neon-fp-armv8", false, \
  "armv8a-linux-gnueabihf", UC_CPU_ARM_MAX

// clang-format off

static const std::vector<RoundTripTC> kArm32AcqRel = {

  // LDAB: zero-extended byte load.  m[0]=0x11 -> 0x11 (4-byte load = 0x44332211).
  {"ldab",
   "unsigned long ldab(unsigned long a){ unsigned char m[4]={0x11,0x22,0x33,0x44}; unsigned r;"
   " __asm__ volatile(\"ldab %0,[%1]\":\"=r\"(r):\"r\"(m):\"memory\"); return r; }\n",
   {0}, A32V8},

  // LDAH: zero-extended halfword load.  -> 0x2211 (4-byte load = 0x44332211).
  {"ldah",
   "unsigned long ldah(unsigned long a){ unsigned char m[4]={0x11,0x22,0x33,0x44}; unsigned r;"
   " __asm__ volatile(\"ldah %0,[%1]\":\"=r\"(r):\"r\"(m):\"memory\"); return r; }\n",
   {0}, A32V8},

  // LDREXB: zero-extended byte exclusive load (load value is monitor-independent).
  {"ldrexb",
   "unsigned long ldrexb(unsigned long a){ unsigned char m[4]={0xAA,0xBB,0xCC,0xDD}; unsigned r;"
   " __asm__ volatile(\"ldrexb %0,[%1]\":\"=r\"(r):\"r\"(m):\"memory\"); return r; }\n",
   {0}, A32V8},

  // STLB: store low byte only.  *p=0x11223344, store 0xDD -> 0x112233DD
  // (4-byte store = 0xAABBCCDD).  Uses a pointer arg (DATA region, not a stack
  // slot) so the i32-init / i8-store / i32-load sequence is plain memory the
  // backend forwards correctly — independent of the stack-slot model.
  {"stlb",
   "unsigned long stlb(unsigned long a, unsigned *p){ *p=0x11223344;"
   " __asm__ volatile(\"stlb %0,[%1]\"::\"r\"((unsigned)a),\"r\"(p):\"memory\"); return *p; }\n",
   {0xAABBCCDD, 0x500000}, A32V8},

  // STLH: store low halfword only.  *p=0x11223344, store 0xCCDD -> 0x1122CCDD.
  {"stlh",
   "unsigned long stlh(unsigned long a, unsigned *p){ *p=0x11223344;"
   " __asm__ volatile(\"stlh %0,[%1]\"::\"r\"((unsigned)a),\"r\"(p):\"memory\"); return *p; }\n",
   {0xAABBCCDD, 0x500000}, A32V8},

  // STLB into a stack local, read back the whole word.  Exercises the byte
  // store width AND the epilogue `pop {r0}` return-value load end-to-end (this
  // case previously returned the argument because the single-register pop was
  // dropped, not because of any store-to-load forwarding issue).
  {"stlb_stack",
   "unsigned long stlb_stack(unsigned long a){ unsigned m=0x11223344;"
   " __asm__ volatile(\"stlb %0,[%1]\"::\"r\"((unsigned)a),\"r\"(&m):\"memory\"); return m; }\n",
   {0xAABBCCDD}, A32V8},

  // Controls: word forms already correct.
  {"lda_ctl",
   "unsigned long lda_ctl(unsigned long a){ unsigned m=0xDEADBEEF; unsigned r;"
   " __asm__ volatile(\"lda %0,[%1]\":\"=r\"(r):\"r\"(&m):\"memory\"); return r; }\n",
   {0}, A32V8},

  {"stl_ctl",
   "unsigned long stl_ctl(unsigned long a){ unsigned m=0;"
   " __asm__ volatile(\"stl %0,[%1]\"::\"r\"(a),\"r\"(&m):\"memory\"); return m; }\n",
   {0xCAFEF00D}, A32V8},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(AcqRelWidth, ARM32AcqRelWidthRT,
                         ::testing::ValuesIn(kArm32AcqRel), rtTCName);
