//===- ELFIntegerExtendedFixture.inc - Assembled extended scalar forms ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent Clang encodings for the physical decoder and semantic tests.
///
//===----------------------------------------------------------------------===//

namespace ppc_fixture {
constexpr uint32_t AddPrefix = 0x06000000;
constexpr uint32_t AddR3Suffix = 0x38600000;
constexpr uint32_t ReservedPrefixes[] = {0x06040000, 0x06400000, 0x04000000};
constexpr uint32_t AddR3R4One = 0x38640001;
// Each independent pair encodes r3 at -1(r4) with R=0.
constexpr struct {
  uint32_t Prefix;
  uint32_t Suffix;
  unsigned Bytes;
  bool Store;
  bool Signed;
} MemoryForms[] = {
    {0x0603ffff, 0x8864ffff, 1, false, false}, // plbz
    {0x0603ffff, 0xa064ffff, 2, false, false}, // plhz
    {0x0603ffff, 0xa864ffff, 2, false, true},  // plha
    {0x0603ffff, 0x8064ffff, 4, false, false}, // plwz
    {0x0403ffff, 0xa464ffff, 4, false, true},  // plwa
    {0x0403ffff, 0xe464ffff, 8, false, false}, // pld
    {0x0603ffff, 0x9864ffff, 1, true, false},  // pstb
    {0x0603ffff, 0xb064ffff, 2, true, false},  // psth
    {0x0603ffff, 0x9064ffff, 4, true, false},  // pstw
    {0x0403ffff, 0xf464ffff, 8, true, false},  // pstd
};
} // namespace ppc_fixture

namespace rv_fixture {
// rd and rs1 are a0 and rs2 is a1 unless the name specifies x0.
// Immediate forms encode shamt=0. Tests insert the desired shift count.
constexpr uint32_t SH1ADD = 0x20b52533;
constexpr uint32_t SH2ADD = 0x20b54533;
constexpr uint32_t SH3ADD = 0x20b56533;
constexpr uint32_t SH3ADDZero = 0x20b06533;
constexpr uint32_t AddUW = 0x08b5053b;
constexpr uint32_t SH1ADDUW = 0x20b5253b;
constexpr uint32_t SH2ADDUW = 0x20b5453b;
constexpr uint32_t SH3ADDUW = 0x20b5653b;
constexpr uint32_t SLLIUW = 0x0805151b;
constexpr uint32_t BCLR = 0x48b51533;
constexpr uint32_t BEXT = 0x48b55533;
constexpr uint32_t BINV = 0x68b51533;
constexpr uint32_t BSET = 0x28b51533;
constexpr uint32_t BSETZero = 0x28051533;
constexpr uint32_t BCLRI = 0x48051513;
constexpr uint32_t BEXTI = 0x48055513;
constexpr uint32_t BINVI = 0x68051513;
constexpr uint32_t BSETI = 0x28051513;
// RV64 word operations and Zbs immediate forms with shamt[5] set.
constexpr uint32_t ReservedRV32[] = {AddUW,      SH1ADDUW,   0x0bf5151b,
                                     0x2a051513, 0x4a051513, 0x4a055513,
                                     0x6a051513};
} // namespace rv_fixture
