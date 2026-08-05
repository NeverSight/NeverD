//===- X64_SubregStressTests.cpp - Sub-register aliasing stress tests ----===//
//
// Targeted tests for x86_64 sub-register aliasing correctness.
// These exercise every combination of wide-write/narrow-read and
// narrow-write/wide-read patterns to ensure the optimizer correctly
// models register dependencies across different sizes.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

TEST_P(X64RoundTrip, SubregStress) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kX64SubregStress = {

  // --- Pattern 1: Write RAX (64-bit), read AL (8-bit low byte) ---
  // MOVZX uses this — already verified. Test via C cast.
  {"subreg_rax_to_byte",
   "long subreg_rax_to_byte(long a) { return (unsigned char)a; }\n",
   {0x12345678ABCDEF42ULL}, "SubregRT"},

  // --- Pattern 2: 32-bit op on 64-bit value (natural zero-extension) ---
  {"subreg_eax_zext",
   "typedef unsigned int u32;\n"
   "long subreg_eax_zext(long a) { return (u32)(a + 1); }\n",
   {0xFFFFFFFF}, "SubregRT"},

  // --- Pattern 3: 64-bit → 16-bit truncation ---
  {"subreg_rax_to_word",
   "long subreg_rax_to_word(long a) { return (unsigned short)a; }\n",
   {0x123456789ABCDEF0ULL}, "SubregRT"},

  // --- Pattern 4: Multiple sub-register operations in sequence ---
  {"subreg_chain_byte_ops",
   "long subreg_chain_byte_ops(long a, long b) {\n"
   "  unsigned char lo_a = (unsigned char)a;\n"
   "  unsigned char lo_b = (unsigned char)b;\n"
   "  return lo_a + lo_b;\n"
   "}\n",
   {0x42, 0x37}, "SubregRT"},

  // --- Pattern 5: Sign extension from byte ---
  {"subreg_sext_byte",
   "long subreg_sext_byte(long a) { return (signed char)a; }\n",
   {0x80}, "SubregRT"},

  // --- Pattern 6: Sign extension from word ---
  {"subreg_sext_word",
   "long subreg_sext_word(long a) { return (short)a; }\n",
   {0x8000}, "SubregRT"},

  // --- Pattern 7: Sign extension from dword ---
  {"subreg_sext_dword",
   "long subreg_sext_dword(long a) { return (int)a; }\n",
   {0xFFFFFFFF}, "SubregRT"},

  // --- Pattern 8: Byte extraction at different positions ---
  {"subreg_byte_shift",
   "long subreg_byte_shift(long a) {\n"
   "  return ((unsigned char)(a >> 8)) + ((unsigned char)a);\n"
   "}\n",
   {0x1234}, "SubregRT"},

  // --- Pattern 9: Mix of 32-bit and 64-bit arithmetic ---
  {"subreg_mixed_width",
   "long subreg_mixed_width(long a, long b) {\n"
   "  int lo = (int)a + (int)b;\n"
   "  return (unsigned int)lo;\n"
   "}\n",
   {0x100000000ULL, 0x200000000ULL}, "SubregRT"},

  // --- Pattern 10: Byte extraction and zero-extension (simple C) ---
  {"subreg_narrow_widen",
   "long subreg_narrow_widen(long a) {\n"
   "  return a & 0xFF;\n"
   "}\n",
   {0xABCDEF42ULL}, "SubregRT"},

  // --- Pattern 11: 32-bit multiply (uses EAX, result zero-extends to RAX) ---
  {"subreg_mul32",
   "long subreg_mul32(long a, long b) {\n"
   "  return (unsigned int)((unsigned int)a * (unsigned int)b);\n"
   "}\n",
   {100000, 100000}, "SubregRT"},

  // --- Pattern 12: Byte comparison ---
  {"subreg_cmp_byte",
   "long subreg_cmp_byte(long a, long b) {\n"
   "  return (unsigned char)a == (unsigned char)b;\n"
   "}\n",
   {0x1042, 0x2042}, "SubregRT"},

  // --- Pattern 13: 64→8 truncation then back to 64 (MOVZX-style) ---
  {"subreg_trunc_zext_cycle",
   "long subreg_trunc_zext_cycle(long a) {\n"
   "  unsigned char b = (unsigned char)a;\n"
   "  return (long)b + 1;\n"
   "}\n",
   {0x12345678ABCDEF42ULL}, "SubregRT"},

  // --- Pattern 14: 64→16 truncation in arithmetic ---
  {"subreg_word_arith",
   "long subreg_word_arith(long a, long b) {\n"
   "  unsigned short sa = (unsigned short)a;\n"
   "  unsigned short sb = (unsigned short)b;\n"
   "  return sa * sb;\n"
   "}\n",
   {300, 200}, "SubregRT"},

  // --- Pattern 15: Nested width reductions ---
  {"subreg_nested_narrow",
   "long subreg_nested_narrow(long a) {\n"
   "  int w = (int)a;\n"
   "  short h = (short)w;\n"
   "  char b = (char)h;\n"
   "  return (unsigned char)b;\n"
   "}\n",
   {0x12345678}, "SubregRT"},

  // --- Pattern 16: Mixed signed/unsigned byte ops ---
  {"subreg_mixed_sign_byte",
   "long subreg_mixed_sign_byte(long a) {\n"
   "  signed char s = (signed char)a;\n"
   "  unsigned char u = (unsigned char)a;\n"
   "  return (long)s + (long)u;\n"
   "}\n",
   {0xFE}, "SubregRT"},

  // --- Pattern 17: 32-bit rotate via shift ---
  {"subreg_rotate32",
   "typedef unsigned int u32;\n"
   "long subreg_rotate32(long a) {\n"
   "  u32 x = (u32)a;\n"
   "  return (x << 7) | (x >> 25);\n"
   "}\n",
   {0xDEADBEEF}, "SubregRT"},

  // --- Pattern 18: Byte extraction via shift+mask ---
  {"subreg_byte2_extract",
   "long subreg_byte2_extract(long a) {\n"
   "  return (a >> 16) & 0xFF;\n"
   "}\n",
   {0x00AB0000ULL}, "SubregRT"},

  // --- Pattern 19: 32→64 with overflow ---
  {"subreg_u32_overflow",
   "typedef unsigned int u32;\n"
   "long subreg_u32_overflow(long a, long b) {\n"
   "  u32 r = (u32)a + (u32)b;\n"
   "  return r;\n"
   "}\n",
   {0xFFFFFFFF, 1}, "SubregRT"},

  // --- Pattern 20: Conditional byte select ---
  {"subreg_cond_byte",
   "long subreg_cond_byte(long a, long b) {\n"
   "  unsigned char la = (unsigned char)a;\n"
   "  unsigned char lb = (unsigned char)b;\n"
   "  return la > lb ? la : lb;\n"
   "}\n",
   {0x42, 0x37}, "SubregRT"},

  // --- Pattern 21: 16-bit sign extension then 32-bit use ---
  {"subreg_i16_to_i32_arith",
   "long subreg_i16_to_i32_arith(long a) {\n"
   "  int x = (short)a;\n"
   "  return x * 3;\n"
   "}\n",
   {0x8001}, "SubregRT"},

  // --- Pattern 22: Chain of 32-bit ops preserving zext ---
  {"subreg_chain_u32",
   "typedef unsigned int u32;\n"
   "long subreg_chain_u32(long a, long b) {\n"
   "  u32 x = (u32)a;\n"
   "  x = x + 1;\n"
   "  x = x ^ (u32)b;\n"
   "  x = x & 0xFF00FF;\n"
   "  return x;\n"
   "}\n",
   {0xDEADBEEF, 0x12345678}, "SubregRT"},

  // --- Pattern 23: Byte array simulation ---
  {"subreg_byte_pack",
   "long subreg_byte_pack(long a, long b) {\n"
   "  unsigned char la = (unsigned char)a;\n"
   "  unsigned char lb = (unsigned char)b;\n"
   "  return ((long)la << 8) | lb;\n"
   "}\n",
   {0x42, 0x37}, "SubregRT"},

  // --- Pattern 24: Shift amount in byte (CL-style) ---
  {"subreg_shift_by_byte",
   "long subreg_shift_by_byte(long a, long b) {\n"
   "  unsigned char shift = (unsigned char)b;\n"
   "  return a << (shift & 63);\n"
   "}\n",
   {1, 10}, "SubregRT"},

  // --- Patterns 25-28: explicit MOVZX/MOVSX/MOVSXD after a full-width write,
  // via inline asm.  clang never emits this "write RAX, read AL" alias pattern
  // from C casts, so it exercises a path the casts above cannot reach. ---
  {"subreg_asm_movzx_al",
   "long subreg_asm_movzx_al(long a){ long r;\n"
   "  __asm__ volatile(\"movq %[a],%%rax\\n\\taddq $5,%%rax\\n\\t\"\n"
   "    \"movzbl %%al,%%eax\\n\\tmovq %%rax,%[r]\"\n"
   "    : [r]\"=r\"(r) : [a]\"r\"(a) : \"rax\"); return r; }\n",
   {0x12345678ABCDEF42ULL}, "SubregRT"},
  {"subreg_asm_movsx_al",
   "long subreg_asm_movsx_al(long a){ long r;\n"
   "  __asm__ volatile(\"movq %[a],%%rax\\n\\taddq $3,%%rax\\n\\t\"\n"
   "    \"movsbq %%al,%%rax\\n\\tmovq %%rax,%[r]\"\n"
   "    : [r]\"=r\"(r) : [a]\"r\"(a) : \"rax\"); return r; }\n",
   {0x11223344AABBCC80ULL}, "SubregRT"},
  {"subreg_asm_movzx_ax",
   "long subreg_asm_movzx_ax(long a){ long r;\n"
   "  __asm__ volatile(\"movq %[a],%%rax\\n\\taddq $7,%%rax\\n\\t\"\n"
   "    \"movzwl %%ax,%%eax\\n\\tmovq %%rax,%[r]\"\n"
   "    : [r]\"=r\"(r) : [a]\"r\"(a) : \"rax\"); return r; }\n",
   {0xFFFFFFFFFFFF8000ULL}, "SubregRT"},
  {"subreg_asm_movsxd",
   "long subreg_asm_movsxd(long a){ long r;\n"
   "  __asm__ volatile(\"movq %[a],%%rax\\n\\taddq $1,%%rax\\n\\t\"\n"
   "    \"movslq %%eax,%%rax\\n\\tmovq %%rax,%[r]\"\n"
   "    : [r]\"=r\"(r) : [a]\"r\"(a) : \"rax\"); return r; }\n",
   {0x00000000FFFFFFFFULL}, "SubregRT"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(SubregRT, X64RoundTrip,
                         ::testing::ValuesIn(kX64SubregStress), rtTCName);
