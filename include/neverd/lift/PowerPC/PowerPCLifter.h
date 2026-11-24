//===- PowerPCLifter.h - Classic PowerPC integer lifting --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Scalar instructions in either guest byte order.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIFT_POWERPC_POWERPCLIFTER_H
#define NEVERD_LIFT_POWERPC_POWERPCLIFTER_H

#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/IntegerControl.h"

#include <capstone/capstone.h>
#include <vector>

namespace neverd::ppc {
IntegerControl control(Arch Target, const cs_insn &Insn);
void lift(Arch Target, const cs_insn &Insn, std::vector<LowOp> &Ops);
} // namespace neverd::ppc
#endif // NEVERD_LIFT_POWERPC_POWERPCLIFTER_H
