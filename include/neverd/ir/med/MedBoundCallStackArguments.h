//===- MedBoundCallStackArguments.h - Bound stack arguments ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The stack arguments of a call whose integer contract binds them before SSA
/// (MedOp::IntegerCallSignature) are loads of the caller's outgoing slots at
/// the call.  Where the block itself stored a slot last, the call passes the
/// stored value, as a call whose arguments ABI recovery collects from that
/// store does.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDBOUNDCALLSTACKARGUMENTS_H
#define NEVERD_IR_MED_MEDBOUNDCALLSTACKARGUMENTS_H

#include "neverd/ir/med/MedIR.h"

namespace neverd {

/// Replace each stack operand of a bound integer call that loads the slot the
/// same block last stored, at the same stack pointer value, offset and width,
/// with the stored value.  Only operations that write no memory may lie
/// between the store and the load: register operations, a store to a
/// disjoint slot of the same stack pointer value, and an intrinsic that writes
/// no register and no memory (preservesCallSetup).  Returns whether any
/// operand changed.
bool forwardBoundCallStackArguments(MedFunc &Func);

} // namespace neverd

#endif // NEVERD_IR_MED_MEDBOUNDCALLSTACKARGUMENTS_H
