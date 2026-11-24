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
namespace ppc {
constexpr uint64_t LR = PC + 8;
constexpr uint64_t CTR = LR + 8;
constexpr uint64_t XER = CTR + 8;
// Architectural CR bit zero is the most significant bit of CR0.
constexpr uint64_t crBit(unsigned BI) { return XER + 8 + BI; }
} // namespace ppc
} // namespace neverd::integerreg
#endif // NEVERD_LIFT_INTEGERREGISTERS_H
