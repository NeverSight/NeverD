//===- X64_BitTestMemOffsetRTTests.cpp - BT* memory bit-string addr -*- C++ -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// For a MEMORY bit base, x86 BT/BTS/BTR/BTC do NOT mask the bit offset to the
// operand width.  Instead the offset is a SIGNED bit index into a bit string,
// and the processor (Intel SDM pseudocode) does:
//
//   effective_addr += (BitOffset SAR log2(opbits)) * opbytes   // signed!
//   in_chunk_bit   =  BitOffset AND (opbits - 1)
//
// The crucial, easy-to-break property is that the chunk index uses an ARITHMETIC
// (signed) shift, so a NEGATIVE bit offset floor-divides toward minus infinity
// and the access reaches BACKWARDS in memory, while the in-chunk bit is the
// (always non-negative) remainder.  E.g. `btl $-33, (mem)` with a 4-byte operand
// touches dword (mem-8) bit 31, not "mem bit -33" and not a saturated mem bit 0.
//
// The lifter implements this with INT_SEXT(offset) -> INT_ASHR by (log+3) ->
// INT_LEFT by log -> add to EA, then INT_AND for the in-chunk bit.  The existing
// X64_BitTest suite only drives REGISTER bases (masked) and POSITIVE 32-bit
// memory offsets, so three things had zero roundtrip coverage:
//   * NEGATIVE memory offsets (the signed-shift backwards-reach path),
//   * 64-bit memory operands (log2=6, *8 chunk stride),
//   * BTC with a memory operand (any offset) — the XOR write-back path.
//
// Each modifying probe (BTS/BTR/BTC) folds the WHOLE backing array into the
// return value, so writing the wrong chunk (bad sign, bad stride, truncating
// instead of flooring) lands on a different element and diverges; the read probe
// seeds the target bit in exactly one element so a mis-addressed CF is observed.
// The offset comes from a runtime arg and the asm is volatile, so clang emits
// the register-offset memory form and cannot fold it.  All forms are base-ISA
// and native on the default Unicorn x86-64 CPU.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64BitTestMemOffsetRT : public SemanticRoundTripFixture,
                              public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64BitTestMemOffsetRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
// 6-dword array, op on arr[3]; signed offset reaches arr[0..5].  Fold all lanes.
#define DWORD_BODY(MNEM) \
  "unsigned long f(long a, long b){\n" \
  "  volatile unsigned int arr[6];\n" \
  "  for(int i=0;i<6;i++) arr[i]=(unsigned)(a*(i*0x9E3779B1u+1u)+i);\n" \
  "  int n=(int)b;\n" \
  "  __asm__ volatile(\"" MNEM " %[n], %[m]\"\n" \
  "    :[m]\"+m\"(arr[3]):[n]\"r\"(n):\"cc\",\"memory\");\n" \
  "  unsigned long s=0; for(int i=0;i<6;i++) s=s*1000003UL+arr[i];\n" \
  "  return s;}\n"

// 6-qword array, op on arr[3]; fold all lanes.
#define QWORD_BODY(MNEM) \
  "unsigned long f(long a, long b){\n" \
  "  volatile unsigned long arr[6];\n" \
  "  for(int i=0;i<6;i++) arr[i]=(unsigned long)(a*(i*0x9E3779B97F4A7C15UL+1UL)+i);\n" \
  "  long n=b;\n" \
  "  __asm__ volatile(\"" MNEM " %[n], %[m]\"\n" \
  "    :[m]\"+m\"(arr[3]):[n]\"r\"(n):\"cc\",\"memory\");\n" \
  "  unsigned long s=0; for(int i=0;i<6;i++) s=s*1000003UL+arr[i];\n" \
  "  return s;}\n"

static const std::vector<RoundTripTC> kX64 = {

  // ===== Negative 32-bit memory offsets (signed SAR, backwards reach). =====
  // -33 -> dword (arr[3]-8)=arr[1], bit 31.
  {"bts_mem_neg33", DWORD_BODY("btsl"), {0x0123456789ABCDEFULL, (uint64_t)-33}, "BitTestMemOff", 0},
  // -40 -> arr[1], bit 24  (-40 = -2*32 + 24).
  {"btr_mem_neg40", DWORD_BODY("btrl"), {0x0123456789ABCDEFULL, (uint64_t)-40}, "BitTestMemOff", 0},
  // -65 -> arr[0], bit 31  (-65 = -3*32 + 31).
  {"btc_mem_neg65", DWORD_BODY("btcl"), {0x1122334455667788ULL, (uint64_t)-65}, "BitTestMemOff", 0},

  // ===== BTC with a memory operand (XOR write-back) — POSITIVE offset too. =====
  // +40 -> arr[4], bit 8.
  {"btc_mem_pos40", DWORD_BODY("btcl"), {0xFEDCBA9876543210ULL, 40},  "BitTestMemOff", 0},
  // +33 -> arr[4], bit 1.
  {"bts_mem_pos33", DWORD_BODY("btsl"), {0x00000000DEADBEEFULL, 33},  "BitTestMemOff", 0},

  // ===== 64-bit memory operands (log2=6, *8 stride). =====
  // -1  -> qword arr[2], bit 63.
  {"bts_mem_q_neg1", QWORD_BODY("btsq"), {0x0123456789ABCDEFULL, (uint64_t)-1},  "BitTestMemOff", 0},
  // +64 -> qword arr[4], bit 0.
  {"btr_mem_q_pos64", QWORD_BODY("btrq"), {0x1122334455667788ULL, 64}, "BitTestMemOff", 0},
  // -70 -> qword arr[1], bit 58  (-70 = -2*64 + 58).
  {"btc_mem_q_neg70", QWORD_BODY("btcq"), {0x8000000000000001ULL, (uint64_t)-70}, "BitTestMemOff", 0},
  // +130 -> qword arr[5], bit 2 (130 = 2*64 + 2).
  {"bts_mem_q_pos130", QWORD_BODY("btsq"), {0xCAFEF00DBAADF00DULL, 130}, "BitTestMemOff", 0},

  // ===== Read-only BT with a negative memory offset: CF must come from the
  //       backwards-addressed element.  Seed bit 31 only in arr[1] (the correct
  //       target for -33), so a mis-addressed load reads a clear bit (CF=0). =====
  {"bt_mem_neg33_cf",
   "unsigned long f(long a, long b){\n"
   "  volatile unsigned int arr[6];\n"
   "  for(int i=0;i<6;i++) arr[i]=((unsigned)a+(unsigned)i*0x01010101u)&0x7FFFFFFFu;\n"
   "  arr[1]|=0x80000000u;\n"               // bit 31 set ONLY in arr[1]
   "  int n=(int)b; unsigned long cf=0;\n"
   "  __asm__ volatile(\"btl %[n], %[m]\\n\\tsetc %b[c]\"\n"
   "    :[c]\"+r\"(cf):[m]\"m\"(arr[3]),[n]\"r\"(n):\"cc\",\"memory\");\n"
   "  return (cf&1);}\n",
   {0x12345678ULL, (uint64_t)-33}, "BitTestMemOff", 0},

  // Read-only BT, 64-bit, positive multi-chunk: +70 -> qword arr[4], bit 6.
  {"bt_mem_q_pos70_cf",
   "unsigned long f(long a, long b){\n"
   "  volatile unsigned long arr[6];\n"
   "  for(int i=0;i<6;i++) arr[i]=((unsigned long)a+(unsigned long)i*0x0101010101010101UL)&0x7FFFFFFFFFFFFFFFUL;\n"
   "  arr[4]|=(1UL<<6);\n"                   // bit 6 set ONLY in arr[4]
   "  long n=b; unsigned long cf=0;\n"
   "  __asm__ volatile(\"btq %[n], %[m]\\n\\tsetc %b[c]\"\n"
   "    :[c]\"+r\"(cf):[m]\"m\"(arr[3]),[n]\"r\"(n):\"cc\",\"memory\");\n"
   "  return (cf&1);}\n",
   {0x0123456789ABCDEFULL, 70}, "BitTestMemOff", 0},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(BitTestMemOff, X64BitTestMemOffsetRT,
                         ::testing::ValuesIn(kX64), rtTCName);
