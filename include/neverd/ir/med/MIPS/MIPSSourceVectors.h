//===- MIPSSourceVectors.h - Qualified scalar MSA source leaves ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep unsupported vector entry/call and floating callee-state contracts out
/// of ordinary scalar C signatures. This is separate from instruction lift.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MIPS_MIPSSOURCEVECTORS_H
#define NEVERD_IR_MED_MIPS_MIPSSOURCEVECTORS_H

#include "neverd/Common.h"

namespace neverd {
struct BinaryImage;
struct LowFunc;

/// Refuse unqualified vector-bearing source functions before SSA/DCE can hide
/// their incoming machine state or a clobbered callee-preserved FPR view.
void requireMIPSSourceVectors(const LowFunc &Function, Arch Target,
                              const BinaryImage *Image);

} // namespace neverd

#endif // NEVERD_IR_MED_MIPS_MIPSSOURCEVECTORS_H
