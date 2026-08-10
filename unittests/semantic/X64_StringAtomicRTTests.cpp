//===- X64_StringAtomicRTTests.cpp - String/Atomic roundtrip ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests x86_64 string operation patterns and atomic operations through
// the full lift pipeline using C expressions.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64StrAtomRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64StrAtomRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kX64StrAtom = {
  // ========== String-like byte patterns ==========
  {"str_strlen",
   "long str_strlen(long a) {\n"
   "  unsigned char *p = (unsigned char *)(long[]){a};\n"
   "  long len = 0;\n"
   "  while (p[len] && len < 8) ++len;\n"
   "  return len;\n"
   "}\n",
   {0x4142434400ULL}, "StrAtomRT"},

  {"str_count_ones",
   "long str_count_ones(long a) {\n"
   "  long c = 0;\n"
   "  for (int i = 0; i < 8; ++i)\n"
   "    if ((a >> (i*8)) & 0xFF) ++c;\n"
   "  return c;\n"
   "}\n",
   {0x00FF00FF00FF00FFULL}, "StrAtomRT"},

  {"str_byte_swap_pairs",
   "long str_byte_swap_pairs(long a) {\n"
   "  long r = 0;\n"
   "  for (int i = 0; i < 4; ++i) {\n"
   "    long b0 = (a >> (i*16)) & 0xFF;\n"
   "    long b1 = (a >> (i*16+8)) & 0xFF;\n"
   "    r |= (b0 << (i*16+8)) | (b1 << (i*16));\n"
   "  }\n"
   "  return r;\n"
   "}\n",
   {0x0102030405060708ULL}, "StrAtomRT"},

  {"str_find_byte",
   "long str_find_byte(long val, long needle) {\n"
   "  for (int i = 0; i < 8; ++i)\n"
   "    if (((val >> (i*8)) & 0xFF) == (needle & 0xFF))\n"
   "      return i;\n"
   "  return -1;\n"
   "}\n",
   {0x4142434445464748ULL, 0x44}, "StrAtomRT"},

  {"str_replace_byte",
   "long str_replace_byte(long val, long pos) {\n"
   "  long mask = ~(0xFFULL << (pos * 8));\n"
   "  return (val & mask) | (0xAAULL << (pos * 8));\n"
   "}\n",
   {0x0102030405060708ULL, 3}, "StrAtomRT"},

  // ========== Atomic-like CAS patterns (via C expressions) ==========
  {"atom_cmpxchg_sim",
   "long atom_cmpxchg_sim(long current, long expected) {\n"
   "  return current == expected ? 1 : 0;\n"
   "}\n",
   {42, 42}, "StrAtomRT"},

  {"atom_fetch_add_sim",
   "long atom_fetch_add_sim(long val, long inc) {\n"
   "  return val + inc;\n"
   "}\n",
   {100, 42}, "StrAtomRT"},

  {"atom_fetch_and_sim",
   "long atom_fetch_and_sim(long val, long mask) {\n"
   "  return val & mask;\n"
   "}\n",
   {0xFF00FF00FF00FF00ULL, 0x0FF00FF00FF00FF0ULL}, "StrAtomRT"},

  {"atom_fetch_or_sim",
   "long atom_fetch_or_sim(long val, long mask) {\n"
   "  return val | mask;\n"
   "}\n",
   {0xF0F0F0F0F0F0F0F0ULL, 0x0F0F0F0F0F0F0F0FULL}, "StrAtomRT"},

  {"atom_fetch_xor_sim",
   "long atom_fetch_xor_sim(long val, long mask) {\n"
   "  return val ^ mask;\n"
   "}\n",
   {0xFF00FF00FF00FF00ULL, 0x0FF00FF00FF00FF0ULL}, "StrAtomRT"},

  {"atom_xchg_sim",
   "long atom_xchg_sim(long old, long new_val) {\n"
   "  return new_val;\n"
   "}\n",
   {42, 100}, "StrAtomRT"},

  // ========== Bit scan / extraction patterns ==========
  {"bit_extract_field",
   "long bit_extract_field(long val, long pos) {\n"
   "  return (val >> pos) & 0xF;\n"
   "}\n",
   {0xDEADBEEFCAFEBABEULL, 12}, "StrAtomRT"},

  {"bit_deposit_field",
   "long bit_deposit_field(long val, long field) {\n"
   "  return (val & ~0xF0ULL) | ((field & 0xF) << 4);\n"
   "}\n",
   {0xFF00, 0xA}, "StrAtomRT"},

  {"bit_count_trailing_zeros",
   "long bit_count_trailing_zeros(long a) {\n"
   "  if (a == 0) return 64;\n"
   "  long c = 0;\n"
   "  while ((a & 1) == 0) { a >>= 1; ++c; }\n"
   "  return c;\n"
   "}\n",
   {0x100}, "StrAtomRT"},

  {"bit_isolate_lowest",
   "long bit_isolate_lowest(long a) {\n"
   "  return a & (-(long)a);\n"
   "}\n",
   {0xDEADBEEF00ULL}, "StrAtomRT"},

  {"bit_clear_lowest",
   "long bit_clear_lowest(long a) {\n"
   "  return a & (a - 1);\n"
   "}\n",
   {0xDEADBEEF00ULL}, "StrAtomRT"},

  // ========== Hash / mixing patterns ==========
  {"hash_murmurmix",
   "long hash_murmurmix(long h) {\n"
   "  h ^= h >> 33;\n"
   "  h *= 0xff51afd7ed558ccdULL;\n"
   "  h ^= h >> 33;\n"
   "  h *= 0xc4ceb9fe1a85ec53ULL;\n"
   "  h ^= h >> 33;\n"
   "  return h;\n"
   "}\n",
   {42}, "StrAtomRT"},

  {"hash_fnv1a_byte",
   "long hash_fnv1a_byte(long hash, long byte) {\n"
   "  hash ^= (byte & 0xFF);\n"
   "  hash *= 0x100000001B3ULL;\n"
   "  return hash;\n"
   "}\n",
   {0xCBF29CE484222325ULL, 0x41}, "StrAtomRT"},

  // ========== Memory pattern simulation ==========
  {"mem_pack_4x16",
   "long mem_pack_4x16(long a, long b) {\n"
   "  return ((a & 0xFFFF) | ((b & 0xFFFF) << 16) |\n"
   "          ((a >> 16 & 0xFFFF) << 32) | ((b >> 16 & 0xFFFF) << 48));\n"
   "}\n",
   {0x00010002ULL, 0x00030004ULL}, "StrAtomRT"},

  {"mem_unpack_even_bytes",
   "long mem_unpack_even_bytes(long a) {\n"
   "  long r = 0;\n"
   "  for (int i = 0; i < 4; ++i)\n"
   "    r |= ((a >> (i*16)) & 0xFF) << (i*8);\n"
   "  return r;\n"
   "}\n",
   {0x0102030405060708ULL}, "StrAtomRT"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(StrAtomRT, X64StrAtomRT,
                         ::testing::ValuesIn(kX64StrAtom), rtTCName);
