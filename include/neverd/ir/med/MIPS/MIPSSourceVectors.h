//===- MIPSSourceVectors.h - Qualified scalar/vector source contracts
//----------===//
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
#include "neverd/ir/FloatingCallArguments.h"

#include <map>

namespace neverd {
struct BinaryImage;
struct LowFunc;
struct MedFunc;

/// Refuse unqualified vector-bearing source functions before SSA/DCE can hide
/// their incoming machine state or a clobbered callee-preserved FPR view.
void requireMIPSSourceVectors(
    const LowFunc &Function, Arch Target, const BinaryImage *Image,
    const std::map<va_t, FloatingCallArguments> *Calls = nullptr);

/// A scalar source result observes only its ABI width. Canonicalize an
/// unpredictable value only after proving every transported byte unobserved;
/// live unpredictable bytes retain their diagnostic intrinsic.
void discardUnobservedMIPSFPUpperBits(MedFunc &Function, Arch Target);

} // namespace neverd

#endif // NEVERD_IR_MED_MIPS_MIPSSOURCEVECTORS_H
