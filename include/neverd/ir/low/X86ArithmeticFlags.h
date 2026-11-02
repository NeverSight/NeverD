//===- X86ArithmeticFlags.h - Known arithmetic flag results ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exact arithmetic flags from a classic CMP whose operands are equal.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86ARITHMETICFLAGS_H
#define NEVERD_IR_LOW_X86ARITHMETICFLAGS_H

#include "neverd/lift/X86/X86Regs.h"

#include <array>
#include <utility>

namespace neverd {

/// Equal defined integer operands subtract to zero without borrow or signed
/// overflow. Only these six flags change; DF, vector and x87 state survive.
inline constexpr std::array kX86EqualComparisonFlags = {
    std::pair{x86reg::CF, false}, std::pair{x86reg::PF, true},
    std::pair{x86reg::AF, false}, std::pair{x86reg::ZF, true},
    std::pair{x86reg::SF, false}, std::pair{x86reg::OF, false}};

} // namespace neverd

#endif // NEVERD_IR_LOW_X86ARITHMETICFLAGS_H
