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
// r3 is the transfer register, r4 the base and r5 the index.
constexpr struct {
  uint32_t Word;
  unsigned Bytes;
  bool Store;
} ReversedTransfers[] = {
    {0x7c642e2c, 2, false}, // lhbrx
    {0x7c642c2c, 4, false}, // lwbrx
    {0x7c642c28, 8, false}, // ldbrx
    {0x7c642f2c, 2, true},  // sthbrx
    {0x7c642d2c, 4, true},  // stwbrx
    {0x7c642d28, 8, true},  // stdbrx
};
constexpr uint32_t CnttzwR3R4 = 0x7c830434;
constexpr uint32_t CnttzdR3R4 = 0x7c830474;
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
constexpr uint32_t AndN = 0x40b57533;
constexpr uint32_t OrN = 0x40b56533;
constexpr uint32_t Xnor = 0x40b54533;
constexpr uint32_t Min = 0x0ab54533;
constexpr uint32_t MinU = 0x0ab55533;
constexpr uint32_t Max = 0x0ab56533;
constexpr uint32_t MaxU = 0x0ab57533;
constexpr uint32_t Clz = 0x60051513;
constexpr uint32_t Cpop = 0x60251513;
constexpr uint32_t SextB = 0x60451513;
constexpr uint32_t SextH = 0x60551513;
constexpr uint32_t Ror = 0x60b55533;
constexpr uint32_t RolW = 0x60b5153b;
constexpr uint32_t CpopW = 0x6025151b;
constexpr uint32_t Rori31 = 0x61f55513;
constexpr uint32_t RoriW31 = 0x61f5551b;
constexpr uint32_t RolIntoRs2 = 0x60b515b3;
constexpr uint32_t RorFromZero = 0x60b05533;
constexpr uint32_t RolToZero = 0x60b51033;
constexpr uint32_t OrcB = 0x28755513;
constexpr uint32_t Ctz = 0x60151513;
constexpr uint32_t RorW = 0x60b5553b;
constexpr uint32_t CtzW = 0x6015151b;
constexpr uint32_t ClzW = 0x6005151b;
constexpr uint32_t Rev8_32 = 0x69855513;
constexpr uint32_t Rev8_64 = 0x6b855513;
constexpr uint32_t ZextH32 = 0x08054533;
constexpr uint32_t ZextH64 = 0x0805453b;
constexpr uint32_t Rori63 = 0x63f55513;
constexpr uint32_t ReservedUnary = 0x60351513;
constexpr uint32_t ReservedRoriW = 0x63f5551b; // shamt[5] set
} // namespace rv_fixture
