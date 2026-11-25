//===- PowerPCArithmeticFixture.inc - Assembled scalar arithmetic ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

// popcntb r5,r6; popcntw r5,r6; popcntd r5,r6.
constexpr struct {
  uint32_t Word;
  unsigned LaneBytes;
} PopulationForms[] = {{0x7cc500f4, 1}, {0x7cc502f4, 4}, {0x7cc503f4, 8}};

constexpr uint32_t MultiplyWord = 0x7ca639d6;       // mullw r5,r6,r7
constexpr uint32_t MultiplyWordRecord = 0x7ca639d7; // mullw. r5,r6,r7
