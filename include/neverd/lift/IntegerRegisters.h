//===- IntegerRegisters.h - RISC-V and PowerPC register storage -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Both integer files have 32 numbered GPRs. Offsets denote architectural slots
/// independent of guest byte order.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIFT_INTEGERREGISTERS_H
#define NEVERD_LIFT_INTEGERREGISTERS_H

#include <cstdint>

namespace neverd::integerreg {
constexpr uint64_t gpr(unsigned Index) { return (1 + Index) * 8; }
constexpr uint64_t PC = gpr(32);
namespace riscv {
constexpr uint64_t fpr(unsigned Index) { return PC + (1 + Index) * 8; }
} // namespace riscv
namespace ppc {
constexpr uint64_t LR = PC + 8;
constexpr uint64_t CTR = LR + 8;
constexpr uint64_t XER = CTR + 8;
#define NEVERD_PPC_XER_BIT(Name, Shift) constexpr unsigned XER##Name = Shift;
#define NEVERD_PPC_CR_BIT(Name, BI) constexpr unsigned CR##Name = BI;
#include "neverd/lift/PowerPC/PowerPCRegisterFields.def"
// Architectural CR bit zero is the most significant bit of CR0.
constexpr uint64_t crBit(unsigned BI) { return XER + 8 + BI; }
constexpr unsigned crShift(unsigned BI) { return 31 - BI; }
constexpr uint32_t CR0ComparisonMask = UINT32_C(7) << crShift(CREQ);
} // namespace ppc
} // namespace neverd::integerreg
#endif // NEVERD_LIFT_INTEGERREGISTERS_H
