//===- RISCVPack.inc - Assembled Zbkb test instructions ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

namespace pack_fixture {
constexpr uint32_t Pack = 0x08b54533;          // pack a0,a0,a1
constexpr uint32_t PackH = 0x08b57533;         // packh a0,a0,a1
constexpr uint32_t PackW = 0x08b5453b;         // packw a0,a0,a1
constexpr uint32_t PackIntoHigh = 0x08b545b3;  // pack a1,a0,a1
constexpr uint32_t PackZeroHigh = 0x08054533;  // pack a0,a0,zero
constexpr uint32_t PackWZeroHigh = 0x0805453b; // packw a0,a0,zero
constexpr uint32_t PackHZeroLow = 0x08b07533;  // packh a0,zero,a1
constexpr uint32_t PackDiscard = 0x08b54033;   // pack zero,a0,a1
constexpr uint32_t Brev8 = 0x68755513;         // brev8 a0,a0
} // namespace pack_fixture
