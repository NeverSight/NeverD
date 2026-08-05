//===- ARM32_LdmStmModeRTTests.cpp - LDM/STM addressing modes ----------===//
//
// ARM32 block load/store (`LDM`/`STM`) come in four addressing modes that pick
// where the transfer block sits relative to the base register Rn and whether
// the base steps before or after each word:
//
//   LDM / LDMIA  (Increment After)   addr = Rn, Rn+4, ...          (default)
//   LDMIB        (Increment Before)  addr = Rn+4, Rn+8, ...
//   LDMDA        (Decrement After)   addr = Rn-4*(n-1), ..., Rn
//   LDMDB        (Decrement Before)  addr = Rn-4*n, ..., Rn-4
//   (STM mirrors each; with `!` the base is written back +/- 4*n.)
//
// In ALL modes the LOWEST-numbered register maps to the LOWEST address, so the
// per-register offset start is mode- AND count-dependent:
//   IA: 0          IB: +4
//   DA: -4*(n-1)   DB: -4*n
// Getting the DA/DB start wrong (off by one word, or independent of `n`) writes
// the block one slot high/low or overlapping the base.
//
// Coverage gap: a compiler only ever emits `ldmia`/`stmdb` (the push/pop pair,
// which capstone further aliases to ARM_INS_LDM/STM with the "pop"/"push"
// mnemonic and an implicit SP base).  The explicit `ldmib`/`ldmda`/`ldmdb` and
// `stmib`/`stmda`/`stmia` forms with a NAMED base register — the generic
// ARMLiftMem.cpp path — had ZERO roundtrip coverage (`ldmib`/`stmda` are even
// ARM-state-only: clang rejects them under -mthumb, confirming the fixture
// compiles A32).  These probes drive every mode through the named-base path.
//
// Each probe is invariant to the recompiled function's absolute frame address:
// it folds only (a) buffer CONTENTS (fixed values seeded from the argument so
// clang cannot fold the volatile-asm result) and (b) RELATIVE base deltas
// (p - &buf[k], i.e. the writeback step in words), never an absolute pointer.
// The oracle is original-Unicorn vs lifted-Unicorn return value.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32LdmStmModeRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32LdmStmModeRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kArm32LdmStm = {

  // ===== STM, no writeback: which buffer slots get written per mode. =====
  // base = &b[8]; IA writes b[8..11], IB b[9..12], DA b[5..8], DB b[4..7].
  {"stm_ia",
   "unsigned long f(unsigned long a){ unsigned b[16]; for(int i=0;i<16;i++) b[i]=0x9000u+i;\n"
   " __asm__ volatile(\"mov r4,%[s]\\n add r5,r4,#0x11\\n add r6,r4,#0x22\\n add r7,r4,#0x33\\n stmia %[b],{r4,r5,r6,r7}\\n\"\n"
   "  ::[b]\"r\"(&b[8]),[s]\"r\"((unsigned)a):\"r4\",\"r5\",\"r6\",\"r7\",\"memory\");\n"
   " unsigned acc=0; for(int i=0;i<16;i++) acc=acc*131u+b[i]; return acc; }\n",
   {0x12345678ULL}, "LdmStm", 1, ""},

  {"stm_ib",
   "unsigned long f(unsigned long a){ unsigned b[16]; for(int i=0;i<16;i++) b[i]=0x9000u+i;\n"
   " __asm__ volatile(\"mov r4,%[s]\\n add r5,r4,#0x11\\n add r6,r4,#0x22\\n add r7,r4,#0x33\\n stmib %[b],{r4,r5,r6,r7}\\n\"\n"
   "  ::[b]\"r\"(&b[8]),[s]\"r\"((unsigned)a):\"r4\",\"r5\",\"r6\",\"r7\",\"memory\");\n"
   " unsigned acc=0; for(int i=0;i<16;i++) acc=acc*131u+b[i]; return acc; }\n",
   {0x12345678ULL}, "LdmStm", 1, ""},

  {"stm_da",
   "unsigned long f(unsigned long a){ unsigned b[16]; for(int i=0;i<16;i++) b[i]=0x9000u+i;\n"
   " __asm__ volatile(\"mov r4,%[s]\\n add r5,r4,#0x11\\n add r6,r4,#0x22\\n add r7,r4,#0x33\\n stmda %[b],{r4,r5,r6,r7}\\n\"\n"
   "  ::[b]\"r\"(&b[8]),[s]\"r\"((unsigned)a):\"r4\",\"r5\",\"r6\",\"r7\",\"memory\");\n"
   " unsigned acc=0; for(int i=0;i<16;i++) acc=acc*131u+b[i]; return acc; }\n",
   {0x12345678ULL}, "LdmStm", 1, ""},

  {"stm_db",
   "unsigned long f(unsigned long a){ unsigned b[16]; for(int i=0;i<16;i++) b[i]=0x9000u+i;\n"
   " __asm__ volatile(\"mov r4,%[s]\\n add r5,r4,#0x11\\n add r6,r4,#0x22\\n add r7,r4,#0x33\\n stmdb %[b],{r4,r5,r6,r7}\\n\"\n"
   "  ::[b]\"r\"(&b[8]),[s]\"r\"((unsigned)a):\"r4\",\"r5\",\"r6\",\"r7\",\"memory\");\n"
   " unsigned acc=0; for(int i=0;i<16;i++) acc=acc*131u+b[i]; return acc; }\n",
   {0x12345678ULL}, "LdmStm", 1, ""},

  // ===== LDM, no writeback: read in[] block into r4-r7, mirror to out[] via
  // stmia, fold out[].  base = &in[8]. =====
  {"ldm_ia",
   "unsigned long f(unsigned long a){ unsigned in[16],out[8]; for(int i=0;i<16;i++) in[i]=(unsigned)a*7u+i*0x101u; for(int i=0;i<8;i++) out[i]=0;\n"
   " __asm__ volatile(\"ldmia %[i],{r4,r5,r6,r7}\\n stmia %[o],{r4,r5,r6,r7}\\n\"\n"
   "  ::[i]\"r\"(&in[8]),[o]\"r\"(out):\"r4\",\"r5\",\"r6\",\"r7\",\"memory\");\n"
   " unsigned acc=0; for(int i=0;i<8;i++) acc=acc*131u+out[i]; return acc; }\n",
   {0x0BADF00DULL}, "LdmStm", 1, ""},

  {"ldm_ib",
   "unsigned long f(unsigned long a){ unsigned in[16],out[8]; for(int i=0;i<16;i++) in[i]=(unsigned)a*7u+i*0x101u; for(int i=0;i<8;i++) out[i]=0;\n"
   " __asm__ volatile(\"ldmib %[i],{r4,r5,r6,r7}\\n stmia %[o],{r4,r5,r6,r7}\\n\"\n"
   "  ::[i]\"r\"(&in[8]),[o]\"r\"(out):\"r4\",\"r5\",\"r6\",\"r7\",\"memory\");\n"
   " unsigned acc=0; for(int i=0;i<8;i++) acc=acc*131u+out[i]; return acc; }\n",
   {0x0BADF00DULL}, "LdmStm", 1, ""},

  {"ldm_da",
   "unsigned long f(unsigned long a){ unsigned in[16],out[8]; for(int i=0;i<16;i++) in[i]=(unsigned)a*7u+i*0x101u; for(int i=0;i<8;i++) out[i]=0;\n"
   " __asm__ volatile(\"ldmda %[i],{r4,r5,r6,r7}\\n stmia %[o],{r4,r5,r6,r7}\\n\"\n"
   "  ::[i]\"r\"(&in[8]),[o]\"r\"(out):\"r4\",\"r5\",\"r6\",\"r7\",\"memory\");\n"
   " unsigned acc=0; for(int i=0;i<8;i++) acc=acc*131u+out[i]; return acc; }\n",
   {0x0BADF00DULL}, "LdmStm", 1, ""},

  {"ldm_db",
   "unsigned long f(unsigned long a){ unsigned in[16],out[8]; for(int i=0;i<16;i++) in[i]=(unsigned)a*7u+i*0x101u; for(int i=0;i<8;i++) out[i]=0;\n"
   " __asm__ volatile(\"ldmdb %[i],{r4,r5,r6,r7}\\n stmia %[o],{r4,r5,r6,r7}\\n\"\n"
   "  ::[i]\"r\"(&in[8]),[o]\"r\"(out):\"r4\",\"r5\",\"r6\",\"r7\",\"memory\");\n"
   " unsigned acc=0; for(int i=0;i<8;i++) acc=acc*131u+out[i]; return acc; }\n",
   {0x0BADF00DULL}, "LdmStm", 1, ""},

  // ===== 5-register lists exercise the count-dependent DA/DB offset start. =====
  {"stm_da_5reg",
   "unsigned long f(unsigned long a){ unsigned b[16]; for(int i=0;i<16;i++) b[i]=0x9000u+i;\n"
   " __asm__ volatile(\"mov r4,%[s]\\n add r5,r4,#0x11\\n add r6,r4,#0x22\\n add r7,r4,#0x33\\n add r8,r4,#0x44\\n stmda %[b],{r4,r5,r6,r7,r8}\\n\"\n"
   "  ::[b]\"r\"(&b[8]),[s]\"r\"((unsigned)a):\"r4\",\"r5\",\"r6\",\"r7\",\"r8\",\"memory\");\n"
   " unsigned acc=0; for(int i=0;i<16;i++) acc=acc*131u+b[i]; return acc; }\n",
   {0x24681357ULL}, "LdmStm", 1, ""},

  {"ldm_db_5reg",
   "unsigned long f(unsigned long a){ unsigned in[16],out[8]; for(int i=0;i<16;i++) in[i]=(unsigned)a*3u+i*0x171u; for(int i=0;i<8;i++) out[i]=0;\n"
   " __asm__ volatile(\"ldmdb %[i],{r4,r5,r6,r7,r8}\\n stmia %[o],{r4,r5,r6,r7,r8}\\n\"\n"
   "  ::[i]\"r\"(&in[8]),[o]\"r\"(out):\"r4\",\"r5\",\"r6\",\"r7\",\"r8\",\"memory\");\n"
   " unsigned acc=0; for(int i=0;i<8;i++) acc=acc*131u+out[i]; return acc; }\n",
   {0x24681357ULL}, "LdmStm", 1, ""},

  // ===== Writeback (`!`): base steps +/- 4*n.  Fold the relative word delta
  // (p - &buf[8]) plus the contents. =====
  {"stm_ib_wb",
   "unsigned long f(unsigned long a){ unsigned b[16]; for(int i=0;i<16;i++) b[i]=0x9000u+i; unsigned *p=&b[8];\n"
   " __asm__ volatile(\"mov r4,%[s]\\n add r5,r4,#0x11\\n add r6,r4,#0x22\\n add r7,r4,#0x33\\n stmib %[b]!,{r4,r5,r6,r7}\\n\"\n"
   "  :[b]\"+r\"(p):[s]\"r\"((unsigned)a):\"r4\",\"r5\",\"r6\",\"r7\",\"memory\");\n"
   " long d=(long)(p-&b[8]); unsigned acc=(unsigned)(d&0xFFFF); for(int i=0;i<16;i++) acc=acc*131u+b[i]; return acc; }\n",
   {0x12345678ULL}, "LdmStm", 1, ""},

  {"stm_db_wb",
   "unsigned long f(unsigned long a){ unsigned b[16]; for(int i=0;i<16;i++) b[i]=0x9000u+i; unsigned *p=&b[8];\n"
   " __asm__ volatile(\"mov r4,%[s]\\n add r5,r4,#0x11\\n add r6,r4,#0x22\\n add r7,r4,#0x33\\n stmdb %[b]!,{r4,r5,r6,r7}\\n\"\n"
   "  :[b]\"+r\"(p):[s]\"r\"((unsigned)a):\"r4\",\"r5\",\"r6\",\"r7\",\"memory\");\n"
   " long d=(long)(p-&b[8]); unsigned acc=(unsigned)(d&0xFFFF); for(int i=0;i<16;i++) acc=acc*131u+b[i]; return acc; }\n",
   {0x12345678ULL}, "LdmStm", 1, ""},

  {"stm_da_wb",
   "unsigned long f(unsigned long a){ unsigned b[16]; for(int i=0;i<16;i++) b[i]=0x9000u+i; unsigned *p=&b[8];\n"
   " __asm__ volatile(\"mov r4,%[s]\\n add r5,r4,#0x11\\n add r6,r4,#0x22\\n add r7,r4,#0x33\\n stmda %[b]!,{r4,r5,r6,r7}\\n\"\n"
   "  :[b]\"+r\"(p):[s]\"r\"((unsigned)a):\"r4\",\"r5\",\"r6\",\"r7\",\"memory\");\n"
   " long d=(long)(p-&b[8]); unsigned acc=(unsigned)(d&0xFFFF); for(int i=0;i<16;i++) acc=acc*131u+b[i]; return acc; }\n",
   {0x12345678ULL}, "LdmStm", 1, ""},

  {"ldm_ia_wb",
   "unsigned long f(unsigned long a){ unsigned in[16],out[8]; for(int i=0;i<16;i++) in[i]=(unsigned)a*5u+i*0x131u; for(int i=0;i<8;i++) out[i]=0; unsigned *p=&in[8];\n"
   " __asm__ volatile(\"ldmia %[i]!,{r4,r5,r6,r7}\\n stmia %[o],{r4,r5,r6,r7}\\n\"\n"
   "  :[i]\"+r\"(p):[o]\"r\"(out):\"r4\",\"r5\",\"r6\",\"r7\",\"memory\");\n"
   " long d=(long)(p-&in[8]); unsigned acc=(unsigned)(d&0xFFFF); for(int i=0;i<8;i++) acc=acc*131u+out[i]; return acc; }\n",
   {0x0BADF00DULL}, "LdmStm", 1, ""},

  {"ldm_db_wb",
   "unsigned long f(unsigned long a){ unsigned in[16],out[8]; for(int i=0;i<16;i++) in[i]=(unsigned)a*5u+i*0x131u; for(int i=0;i<8;i++) out[i]=0; unsigned *p=&in[8];\n"
   " __asm__ volatile(\"ldmdb %[i]!,{r4,r5,r6,r7}\\n stmia %[o],{r4,r5,r6,r7}\\n\"\n"
   "  :[i]\"+r\"(p):[o]\"r\"(out):\"r4\",\"r5\",\"r6\",\"r7\",\"memory\");\n"
   " long d=(long)(p-&in[8]); unsigned acc=(unsigned)(d&0xFFFF); for(int i=0;i<8;i++) acc=acc*131u+out[i]; return acc; }\n",
   {0x0BADF00DULL}, "LdmStm", 1, ""},

  {"ldm_ib_wb",
   "unsigned long f(unsigned long a){ unsigned in[16],out[8]; for(int i=0;i<16;i++) in[i]=(unsigned)a*5u+i*0x131u; for(int i=0;i<8;i++) out[i]=0; unsigned *p=&in[8];\n"
   " __asm__ volatile(\"ldmib %[i]!,{r4,r5,r6,r7}\\n stmia %[o],{r4,r5,r6,r7}\\n\"\n"
   "  :[i]\"+r\"(p):[o]\"r\"(out):\"r4\",\"r5\",\"r6\",\"r7\",\"memory\");\n"
   " long d=(long)(p-&in[8]); unsigned acc=(unsigned)(d&0xFFFF); for(int i=0;i<8;i++) acc=acc*131u+out[i]; return acc; }\n",
   {0x0BADF00DULL}, "LdmStm", 1, ""},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(LdmStm, ARM32LdmStmModeRT,
                         ::testing::ValuesIn(kArm32LdmStm), rtTCName);
