//===- RISCVConditionalZero.inc - Assembled Zicond test instructions ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Zicond register overlaps and x0 operands assembled with llvm-mc.
///
///===----------------------------------------------------------------------===//

namespace zicond_fixture {
inline constexpr uint32_t Eqz = 0x0eb55533;         // czero.eqz a0,a0,a1
inline constexpr uint32_t Nez = 0x0eb57533;         // czero.nez a0,a0,a1
inline constexpr uint32_t EqzIntoCond = 0x0eb555b3; // czero.eqz a1,a0,a1
inline constexpr uint32_t NezIntoCond = 0x0eb575b3; // czero.nez a1,a0,a1
inline constexpr uint32_t EqzZeroCond = 0x0e055533; // czero.eqz a0,a0,zero
inline constexpr uint32_t NezZeroCond = 0x0e057533; // czero.nez a0,a0,zero
inline constexpr uint32_t EqzZeroData = 0x0eb05533; // czero.eqz a0,zero,a1
inline constexpr uint32_t NezDiscard = 0x0eb57033;  // czero.nez zero,a0,a1
// Same register fields with a different funct3, OP-32 or OP-IMM.
inline constexpr uint32_t NonZicond[] = {0x0eb54533, 0x0eb5553b, 0x0eb55513};
} // namespace zicond_fixture
