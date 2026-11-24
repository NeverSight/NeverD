//===- GoInlinedRoutine.h - Go routines a call runs in place ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The operations a call into one of the Go routines that run in their
/// caller's place performs: Duff's device and the register argument
/// transfers.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_GOINLINEDROUTINE_H
#define NEVERD_IR_LOW_GOINLINEDROUTINE_H

#include "neverd/Common.h"
#include "neverd/ir/IntegerCallArguments.h"
#include "neverd/ir/low/LowIR.h"

#include <optional>
#include <vector>

namespace neverd {

struct BinaryImage;

/// The operations a call to \p Entry performs, in order, where it lies in a
/// routine that runs in its caller's place (goInlinedRoutineAt): the
/// routine's body from \p Entry up to its return.  Go's compiler calls
/// `runtime.duffzero` and `runtime.duffcopy` at the point that zeroes or
/// copies as many bytes as it needs (cmd/compile's DUFFZERO and DUFFCOPY),
/// and reflection's call helpers call `runtime.unspillArgs` and
/// `runtime.spillArgs` to load and store every argument register; each
/// expects the call to change only the registers its straight-line body
/// writes, so the body stands in for the call.  The return's own operations,
/// which pop what the call pushed, stay out.  Nullopt where \p Entry lies in
/// no such routine, an instruction does not lift, or the body transfers
/// control, runs an intrinsic or moves the stack pointer before it returns,
/// or leaves the routine.
std::optional<std::vector<LowOp>> goInlinedRoutineBody(const BinaryImage &Img,
                                                       va_t Entry);

/// The registers the arm64 routine at \p Entry takes its operands in where
/// it stands alone, when it is one of those routines and its body reads at
/// its entry a general-purpose register neither Go's internal ABI nor the
/// platform passes an argument in: `runtime.spillArgs` stores X0 to X15
/// through the internal/abi.RegArgs X20 points at, `runtime.unspillArgs`
/// loads them from it, and `runtime.duffzero` zeroes the words X20 points
/// at.  Each general-purpose register the body reads before it writes it,
/// but those Go code keeps a role for (goStackABIEntryRegister) and the
/// platform's, in the platform's argument order, then Go's, then the others'
/// in theirs.  Its callers run the body in their place, and one that calls
/// it binds these, as a prototype's operands (FixedIntegerCallArguments).
/// None for an x86-64 routine, whose summary gives the registers it reads
/// (goEntryRegisterOperands).
std::optional<FixedIntegerCallArguments>
goInlinedRoutineOperands(const BinaryImage &Img, va_t Entry);

} // namespace neverd

#endif // NEVERD_IR_LOW_GOINLINEDROUTINE_H
