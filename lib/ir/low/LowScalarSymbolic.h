//===- LowScalarSymbolic.h - Integer semantics --------------*- C++ -*-===//
//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Translate bounded integer operations without supplying operand facts.

#ifndef NEVERD_LIB_IR_LOW_LOWSCALARSYMBOLIC_H
#define NEVERD_LIB_IR_LOW_LOWSCALARSYMBOLIC_H

#include "neverd/ir/NdOps.h"
#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <optional>

namespace neverd {
/// Translate one integer LowIR operation to the shared symbolic bit-vector
/// semantics used by complete-domain jump-table proofs.  Inputs are coerced
/// exactly as the production emitter coerces them; unsupported or
/// architecture-dependent width combinations return nullopt rather than
/// authorizing a range from an approximate expression.
std::optional<symbolic::SymRef>
symbolizeJumpTableIntegerOperation(symbolic::SymContext &Ctx, NdOp Opcode,
                                   uint16_t OutputSize,
                                   llvm::ArrayRef<symbolic::SymRef> RawInputs);

} // namespace neverd

#endif // NEVERD_LIB_IR_LOW_LOWSCALARSYMBOLIC_H
