//===- MIPSSourceAccumulators.h - Defined HI/LO source inputs ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Ordinary C signatures have no implicit incoming MIPS HI/LO state.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MIPS_MIPSSOURCEACCUMULATORS_H
#define NEVERD_IR_MED_MIPS_MIPSSOURCEACCUMULATORS_H

namespace neverd {
struct MedFunc;

/// Check SSA inputs before propagation or return inference can hide an
/// unqualified entry/call accumulator read. The caller supplies MIPS MedIR.
void requireMIPSSourceAccumulators(const MedFunc &Function);

} // namespace neverd

#endif // NEVERD_IR_MED_MIPS_MIPSSOURCEACCUMULATORS_H
