//===- MedNoReturn.h - Whole-program no-return propagation ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the MedIR pass that proves internal functions do not return and
/// transfers that fact to their direct call sites.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDNORETURN_H
#define NEVERD_IR_MED_MEDNORETURN_H

#include "neverd/ir/med/MedIR.h"

#include <vector>

namespace neverd {

/// Prove internal no-return functions from explicit architectural terminators
/// and already-known no-return calls, then mark every direct call to them.
void propagateInternalNoReturn(std::vector<MedFunc> &Funcs, Arch TheArch);

} // namespace neverd

#endif // NEVERD_IR_MED_MEDNORETURN_H
