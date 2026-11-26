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

// slw, slw., srw and srw. with r6/r7 as inputs and r5 as destination.
constexpr uint32_t LogicalWordShifts[] = {0x7cc53830, 0x7cc53831, 0x7cc53c30,
                                          0x7cc53c31};

// The destination aliases the first source. Bit zero selects the record form.
constexpr uint32_t HighWordSigned = 0x7cc63896;   // mulhw r6,r6,r7
constexpr uint32_t HighWordUnsigned = 0x7cc63816; // mulhwu r6,r6,r7
constexpr uint32_t OverflowWord = 0x7cc63dd6;     // mullwo r6,r6,r7
constexpr uint32_t OverflowDouble = 0x7cc63dd2;   // mulldo r6,r6,r7
