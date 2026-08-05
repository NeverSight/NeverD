//===- ARM32_LdrdStrdModeRTTests.cpp - LDRD/STRD addressing modes ------===//
//
// ARM32 `LDRD`/`STRD` (load/store a pair of registers Rt:Rt2 from consecutive
// words at the effective address) support the same rich addressing the single
// `LDR`/`STR` do:
//
//   ldrd r0,r1,[Rn, #imm]      immediate offset
//   ldrd r0,r1,[Rn, Rm]        REGISTER offset (ARM-state only)
//   ldrd r0,r1,[Rn, #imm]!     pre-indexed, base writeback
//   ldrd r0,r1,[Rn], #imm      post-indexed, base writeback
//
// But the `ARMLiftMem.cpp` LDRD/STRD handlers only summed `mem.base + mem.disp`:
//   (1) `mem.index` (the `[Rn, Rm]` register-offset form) was IGNORED -> the
//       address dropped the index register entirely;
//   (2) no `subtracted` sign on the displacement;
//   (3) `Insn->detail->writeback` was never honored -> pre/post-indexed forms
//       loaded/stored the right words but left the base register UNUPDATED
//       (post-index additionally must access [Rn] with the increment applied
//       AFTERWARD, which is fine since capstone puts the post offset in a
//       trailing operand and leaves mem.disp==0).
// The single-register LDR/STR path already did all of this; LDRD/STRD were the
// gap (`ARM_INS_LDRD`/`STRD` shared none of it).  Compilers emit LDRD/STRD for
// `long long` / paired field access almost always as plain `[Rn,#imm]`, so the
// register-offset and writeback forms had ZERO roundtrip coverage.
//
// Probes fold only address-independent quantities: the loaded/stored VALUES
// (seeded from the argument) and the RELATIVE base writeback delta
// (p - &buf[k], in words), never an absolute stack pointer.  Oracle is
// original-Unicorn vs lifted-Unicorn.  Register-offset / writeback probes are
// RED before the fix; plain `[Rn,#imm]` controls are GREEN throughout.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32LdrdStrdModeRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32LdrdStrdModeRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kArm32LdrdStrd = {

  // ===== LDRD register offset [Rn, Rm] (RED: index dropped). =====
  // base=b, idx=8 bytes -> load b[2],b[3]; buggy lift ignores idx -> b[0],b[1].
  {"ldrd_reg_off",
   "unsigned long f(unsigned long a){ unsigned b[8]; for(int i=0;i<8;i++) b[i]=0x1000u+i*0x111u+(unsigned)a; unsigned o0,o1;\n"
   " __asm__ volatile(\"ldrd r4,r5,[%[base],%[idx]]\\n mov %[o0],r4\\n mov %[o1],r5\\n\"\n"
   "  :[o0]\"=&r\"(o0),[o1]\"=&r\"(o1):[base]\"r\"(b),[idx]\"r\"(8u):\"r4\",\"r5\",\"memory\");\n"
   " return o0*131u+o1; }\n",
   {0x0BADCAFEULL}, "LdrdStrd", 1, ""},

  // ===== LDRD register offset with pre-index writeback [Rn, Rm]! (RED). =====
  // base=&b[1], idx=8 -> EA=&b[3], load b[3],b[4], writeback base=&b[3] (delta 2).
  {"ldrd_reg_off_wb",
   "unsigned long f(unsigned long a){ unsigned b[8]; for(int i=0;i<8;i++) b[i]=0x1000u+i*0x111u+(unsigned)a; unsigned o0,o1; unsigned *p=&b[1];\n"
   " __asm__ volatile(\"ldrd r4,r5,[%[base],%[idx]]!\\n mov %[o0],r4\\n mov %[o1],r5\\n\"\n"
   "  :[o0]\"=&r\"(o0),[o1]\"=&r\"(o1),[base]\"+r\"(p):[idx]\"r\"(8u):\"r4\",\"r5\",\"memory\");\n"
   " long d=(long)(p-&b[1]); return o0*131u+o1*17u+(unsigned)(d&0xFFFF); }\n",
   {0x0BADCAFEULL}, "LdrdStrd", 1, ""},

  // ===== LDRD pre-indexed immediate writeback [Rn, #imm]! (RED: no writeback). =====
  // base=&b[1], EA=&b[3], load b[3],b[4]; writeback base=&b[3] (delta 2).
  {"ldrd_preidx_wb",
   "unsigned long f(unsigned long a){ unsigned b[8]; for(int i=0;i<8;i++) b[i]=0x1000u+i*0x111u+(unsigned)a; unsigned o0,o1; unsigned *p=&b[1];\n"
   " __asm__ volatile(\"ldrd r4,r5,[%[base],#8]!\\n mov %[o0],r4\\n mov %[o1],r5\\n\"\n"
   "  :[o0]\"=&r\"(o0),[o1]\"=&r\"(o1),[base]\"+r\"(p)::\"r4\",\"r5\",\"memory\");\n"
   " long d=(long)(p-&b[1]); return o0*131u+o1*17u+(unsigned)(d&0xFFFF); }\n",
   {0x0BADCAFEULL}, "LdrdStrd", 1, ""},

  // ===== LDRD post-indexed [Rn], #imm (RED: no writeback). =====
  // base=&b[2], EA=&b[2], load b[2],b[3]; writeback base=&b[4] (delta 2).
  {"ldrd_postidx",
   "unsigned long f(unsigned long a){ unsigned b[8]; for(int i=0;i<8;i++) b[i]=0x1000u+i*0x111u+(unsigned)a; unsigned o0,o1; unsigned *p=&b[2];\n"
   " __asm__ volatile(\"ldrd r4,r5,[%[base]],#8\\n mov %[o0],r4\\n mov %[o1],r5\\n\"\n"
   "  :[o0]\"=&r\"(o0),[o1]\"=&r\"(o1),[base]\"+r\"(p)::\"r4\",\"r5\",\"memory\");\n"
   " long d=(long)(p-&b[2]); return o0*131u+o1*17u+(unsigned)(d&0xFFFF); }\n",
   {0x0BADCAFEULL}, "LdrdStrd", 1, ""},

  // ===== LDRD negative immediate offset [Rn, #-8] (RED if `subtracted` dropped). =====
  // base=&b[4], EA=&b[2], load b[2],b[3].
  {"ldrd_neg_disp",
   "unsigned long f(unsigned long a){ unsigned b[8]; for(int i=0;i<8;i++) b[i]=0x1000u+i*0x111u+(unsigned)a; unsigned o0,o1;\n"
   " __asm__ volatile(\"ldrd r4,r5,[%[base],#-8]\\n mov %[o0],r4\\n mov %[o1],r5\\n\"\n"
   "  :[o0]\"=&r\"(o0),[o1]\"=&r\"(o1):[base]\"r\"(&b[4]):\"r4\",\"r5\",\"memory\");\n"
   " return o0*131u+o1; }\n",
   {0x0BADCAFEULL}, "LdrdStrd", 1, ""},

  // ===== LDRD plain immediate offset control (GREEN throughout). =====
  {"ldrd_imm_off",
   "unsigned long f(unsigned long a){ unsigned b[8]; for(int i=0;i<8;i++) b[i]=0x1000u+i*0x111u+(unsigned)a; unsigned o0,o1;\n"
   " __asm__ volatile(\"ldrd r4,r5,[%[base],#8]\\n mov %[o0],r4\\n mov %[o1],r5\\n\"\n"
   "  :[o0]\"=&r\"(o0),[o1]\"=&r\"(o1):[base]\"r\"(b):\"r4\",\"r5\",\"memory\");\n"
   " return o0*131u+o1; }\n",
   {0x0BADCAFEULL}, "LdrdStrd", 1, ""},

  // ===== STRD register offset [Rn, Rm] (RED: index dropped). =====
  {"strd_reg_off",
   "unsigned long f(unsigned long a){ unsigned b[8]; for(int i=0;i<8;i++) b[i]=0x7000u+i; unsigned v0=(unsigned)a,v1=(unsigned)a+0x55u;\n"
   " __asm__ volatile(\"mov r4,%[v0]\\n mov r5,%[v1]\\n strd r4,r5,[%[base],%[idx]]\\n\"\n"
   "  ::[base]\"r\"(b),[idx]\"r\"(8u),[v0]\"r\"(v0),[v1]\"r\"(v1):\"r4\",\"r5\",\"memory\");\n"
   " unsigned acc=0; for(int i=0;i<8;i++) acc=acc*131u+b[i]; return acc; }\n",
   {0x12344321ULL}, "LdrdStrd", 1, ""},

  // ===== STRD pre-indexed immediate writeback [Rn, #imm]! (RED: no writeback). =====
  {"strd_preidx_wb",
   "unsigned long f(unsigned long a){ unsigned b[8]; for(int i=0;i<8;i++) b[i]=0x7000u+i; unsigned v0=(unsigned)a,v1=(unsigned)a+0x55u; unsigned *p=&b[1];\n"
   " __asm__ volatile(\"mov r4,%[v0]\\n mov r5,%[v1]\\n strd r4,r5,[%[base],#8]!\\n\"\n"
   "  :[base]\"+r\"(p):[v0]\"r\"(v0),[v1]\"r\"(v1):\"r4\",\"r5\",\"memory\");\n"
   " long d=(long)(p-&b[1]); unsigned acc=(unsigned)(d&0xFFFF); for(int i=0;i<8;i++) acc=acc*131u+b[i]; return acc; }\n",
   {0x12344321ULL}, "LdrdStrd", 1, ""},

  // ===== STRD post-indexed [Rn], #imm (RED: no writeback). =====
  {"strd_postidx",
   "unsigned long f(unsigned long a){ unsigned b[8]; for(int i=0;i<8;i++) b[i]=0x7000u+i; unsigned v0=(unsigned)a,v1=(unsigned)a+0x55u; unsigned *p=&b[2];\n"
   " __asm__ volatile(\"mov r4,%[v0]\\n mov r5,%[v1]\\n strd r4,r5,[%[base]],#8\\n\"\n"
   "  :[base]\"+r\"(p):[v0]\"r\"(v0),[v1]\"r\"(v1):\"r4\",\"r5\",\"memory\");\n"
   " long d=(long)(p-&b[2]); unsigned acc=(unsigned)(d&0xFFFF); for(int i=0;i<8;i++) acc=acc*131u+b[i]; return acc; }\n",
   {0x12344321ULL}, "LdrdStrd", 1, ""},

  // ===== STRD plain immediate offset control (GREEN throughout). =====
  {"strd_imm_off",
   "unsigned long f(unsigned long a){ unsigned b[8]; for(int i=0;i<8;i++) b[i]=0x7000u+i; unsigned v0=(unsigned)a,v1=(unsigned)a+0x55u;\n"
   " __asm__ volatile(\"mov r4,%[v0]\\n mov r5,%[v1]\\n strd r4,r5,[%[base],#8]\\n\"\n"
   "  ::[base]\"r\"(b),[v0]\"r\"(v0),[v1]\"r\"(v1):\"r4\",\"r5\",\"memory\");\n"
   " unsigned acc=0; for(int i=0;i<8;i++) acc=acc*131u+b[i]; return acc; }\n",
   {0x12344321ULL}, "LdrdStrd", 1, ""},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(LdrdStrd, ARM32LdrdStrdModeRT,
                         ::testing::ValuesIn(kArm32LdrdStrd), rtTCName);
