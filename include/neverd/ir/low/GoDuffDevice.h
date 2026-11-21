//===- GoDuffDevice.h - Calls into Go's Duff's device -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The operations a call into one of Go's Duff's device routines performs in
/// its caller's place.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_GODUFFDEVICE_H
#define NEVERD_IR_LOW_GODUFFDEVICE_H

#include "neverd/Common.h"
#include "neverd/ir/low/LowIR.h"

#include <optional>
#include <vector>

namespace neverd {

struct BinaryImage;

/// The operations a call to the Duff's device entry \p Entry performs, in
/// order: the routine's body from \p Entry up to its return.  Go's compiler
/// calls `runtime.duffzero` and `runtime.duffcopy` at the point that zeroes or
/// copies as many bytes as it needs (cmd/compile's DUFFZERO and DUFFCOPY), and
/// expects the call to change only the registers that straight-line body
/// writes, so the body stands in for the call (goDuffDeviceAt).  The return's
/// own operations, which pop what the call pushed, stay out.  Nullopt where
/// \p Entry lies in no such routine, an instruction does not lift, or the body
/// transfers control, runs an intrinsic or moves the stack pointer before it
/// returns, or leaves the routine.
std::optional<std::vector<LowOp>> goDuffDeviceBody(const BinaryImage &Img,
                                                   va_t Entry);

} // namespace neverd

#endif // NEVERD_IR_LOW_GODUFFDEVICE_H
