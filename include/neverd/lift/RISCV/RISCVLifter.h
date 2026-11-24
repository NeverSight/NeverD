//===- RISCVLifter.h - RISC-V integer instruction lifting -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Base and compressed instruction semantics for a declared RISC-V profile.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIFT_RISCV_RISCVLIFTER_H
#define NEVERD_LIFT_RISCV_RISCVLIFTER_H

#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/IntegerControl.h"
#include "neverd/lift/RISCV/RISCVProfile.h"

#include <capstone/capstone.h>
#include <vector>

namespace neverd::riscv {
IntegerControl control(Arch Target, RISCVProfile Profile, const cs_insn &Insn);
void lift(Arch Target, RISCVProfile Profile, const cs_insn &Insn,
          std::vector<LowOp> &Ops);
} // namespace neverd::riscv
#endif // NEVERD_LIFT_RISCV_RISCVLIFTER_H
