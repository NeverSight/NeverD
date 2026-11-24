//===- IntegerControl.h - Integer instruction control flow ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Physical branch identity used by the lifter and function discovery.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIFT_INTEGERCONTROL_H
#define NEVERD_LIFT_INTEGERCONTROL_H

#include "neverd/Common.h"
#include "neverd/ir/NdOps.h"

namespace neverd {
struct IntegerControl {
  NdOp Opcode = NdOp::NOP;
  va_t Target = InvalidVA;

  bool terminates() const {
    return Opcode == NdOp::RETURN || Opcode == NdOp::BRANCH ||
           Opcode == NdOp::INDIR_BR;
  }
};
} // namespace neverd
#endif // NEVERD_LIFT_INTEGERCONTROL_H
