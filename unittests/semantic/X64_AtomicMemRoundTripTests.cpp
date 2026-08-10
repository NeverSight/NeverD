//===- X64_AtomicMemRoundTripTests.cpp - Atomic/memory roundtrip -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Covers: atomic CAS, atomic add, memory copy patterns, string-like ops,
// complex pointer arithmetic, struct-like access, volatile patterns.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64AtomicMemRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64AtomicMemRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kX64AtomicMem = {
  // --- Atomic exchange (lock xchg) ---
  {"atomic_xchg",
   "long atomic_xchg(long a) {\n"
   "  long val = a;\n"
   "  long old = __atomic_exchange_n(&val, a + 1, __ATOMIC_SEQ_CST);\n"
   "  return old;\n"
   "}\n",
   {42}, "AtomMem"},

  // --- Atomic add (lock xadd) ---
  {"atomic_fetch_add",
   "long atomic_fetch_add(long a) {\n"
   "  long val = a;\n"
   "  return __atomic_fetch_add(&val, 10, __ATOMIC_SEQ_CST);\n"
   "}\n",
   {32}, "AtomMem"},

  // --- Atomic exchange (xchg) ---
  {"atomic_exchange",
   "long atomic_exchange(long a) {\n"
   "  long val = a;\n"
   "  return __atomic_exchange_n(&val, 99, __ATOMIC_SEQ_CST);\n"
   "}\n",
   {42}, "AtomMem"},

  // --- Memory copy pattern (byte-by-byte) ---
  {"memcpy_4bytes",
   "long memcpy_4bytes(long a) {\n"
   "  long src = a;\n"
   "  long dst = 0;\n"
   "  char *s = (char*)&src, *d = (char*)&dst;\n"
   "  d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];\n"
   "  return dst;\n"
   "}\n",
   {0x04030201}, "AtomMem"},

  // --- Struct-like field access ---
  {"struct_fields",
   "long struct_fields(long a, long b) {\n"
   "  struct { long x; long y; long z; } s;\n"
   "  s.x = a; s.y = b; s.z = a + b;\n"
   "  return s.x * s.y + s.z;\n"
   "}\n",
   {3, 7}, "AtomMem"},

  // --- Array fill and reduce ---
  {"array_reduce",
   "long array_reduce(long n) {\n"
   "  long arr[8];\n"
   "  for (int i = 0; i < 8; ++i) arr[i] = n + i;\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 8; ++i) sum += arr[i];\n"
   "  return sum;\n"
   "}\n",
   {10}, "AtomMem"},

  // --- Byte extraction patterns (generates movzx/shifts) ---
  {"extract_all_bytes",
   "long extract_all_bytes(long a) {\n"
   "  unsigned long u = (unsigned long)a;\n"
   "  long b0 = u & 0xFF;\n"
   "  long b1 = (u >> 8) & 0xFF;\n"
   "  long b2 = (u >> 16) & 0xFF;\n"
   "  long b3 = (u >> 24) & 0xFF;\n"
   "  return b0 + b1 + b2 + b3;\n"
   "}\n",
   {0x01020304}, "AtomMem"},

  // --- Packed nibble operations ---
  {"nibble_swap",
   "long nibble_swap(long a) {\n"
   "  unsigned long u = (unsigned long)a;\n"
   "  return ((u & 0x0F0F0F0F0F0F0F0FULL) << 4) |\n"
   "         ((u >> 4) & 0x0F0F0F0F0F0F0F0FULL);\n"
   "}\n",
   {0xABCD1234DEADBEEFULL}, "AtomMem"},

  // --- Widening multiply high (imul→shr) ---
  {"widening_mul_hi",
   "long widening_mul_hi(long a, long b) {\n"
   "  return (long)(((unsigned __int128)(unsigned long)a * (unsigned long)b) >> 64);\n"
   "}\n",
   {0x100000000ULL, 0x100000000ULL}, "AtomMem"},

  // --- Complex bit manipulation ---
  {"interleave_bits",
   "long interleave_bits(long a, long b) {\n"
   "  unsigned int x = (unsigned int)a & 0xFFFF;\n"
   "  unsigned int y = (unsigned int)b & 0xFFFF;\n"
   "  unsigned int result = 0;\n"
   "  for (int i = 0; i < 16; ++i) {\n"
   "    result |= ((x >> i) & 1) << (2*i);\n"
   "    result |= ((y >> i) & 1) << (2*i+1);\n"
   "  }\n"
   "  return result;\n"
   "}\n",
   {0xAAAA, 0x5555}, "AtomMem"},

  // --- Recursive-like iterative (exercises call stack emulation) ---
  {"ackermann_small",
   "long ackermann_small(long m, long n) {\n"
   "  while (m > 0) {\n"
   "    if (n == 0) { n = 1; --m; }\n"
   "    else { n = n - 1; }\n"
   "  }\n"
   "  return n + 1;\n"
   "}\n",
   {2, 3}, "AtomMem"},

  // --- Saturating arithmetic ---
  {"sat_sub_u64",
   "long sat_sub_u64(long a, long b) {\n"
   "  unsigned long ua = (unsigned long)a, ub = (unsigned long)b;\n"
   "  return ua > ub ? ua - ub : 0;\n"
   "}\n",
   {100, 200}, "AtomMem"},

  // --- Count leading zeros manual ---
  {"manual_clz",
   "long manual_clz(long a) {\n"
   "  unsigned long x = (unsigned long)a;\n"
   "  if (x == 0) return 64;\n"
   "  long n = 0;\n"
   "  if (x <= 0x00000000FFFFFFFFULL) { n += 32; x <<= 32; }\n"
   "  if (x <= 0x0000FFFFFFFFFFFFULL) { n += 16; x <<= 16; }\n"
   "  if (x <= 0x00FFFFFFFFFFFFFFULL) { n += 8;  x <<= 8; }\n"
   "  if (x <= 0x0FFFFFFFFFFFFFFFULL) { n += 4;  x <<= 4; }\n"
   "  if (x <= 0x3FFFFFFFFFFFFFFFULL) { n += 2;  x <<= 2; }\n"
   "  if (x <= 0x7FFFFFFFFFFFFFFFULL) { n += 1; }\n"
   "  return n;\n"
   "}\n",
   {0x100}, "AtomMem"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(AtomMem, X64AtomicMemRT,
                         ::testing::ValuesIn(kX64AtomicMem), rtTCName);
