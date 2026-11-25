//===- PowerPCPopulationFixture.inc - Assembled population counts ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

// popcntb r5,r6; popcntw r5,r6; popcntd r5,r6.
constexpr struct {
  uint32_t Word;
  unsigned LaneBytes;
} PopulationForms[] = {{0x7cc500f4, 1}, {0x7cc502f4, 4}, {0x7cc503f4, 8}};
