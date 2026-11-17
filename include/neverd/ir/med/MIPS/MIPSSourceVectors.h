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
#include "neverd/ir/SourceCallTypeHint.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <map>
#include <optional>

namespace neverd {
struct BinaryImage;
class DebugContext;
struct LowFunc;
struct MedFunc;
struct MedVar;

/// Refuse unqualified vector-bearing source functions before SSA/DCE can hide
/// their incoming machine state or a clobbered callee-preserved FPR view.
/// ConstantTargets, when supplied, is this function's initially empty cache;
/// the converter reuses the exact occurrence proof for later call binding.
void requireMIPSSourceVectors(
    const LowFunc &Function, Arch Target, const BinaryImage *Image,
    const std::map<va_t, FloatingCallArguments> *Calls = nullptr,
    const std::map<SourceCallOccurrenceKey, SourceCallTypeHint> *Sites =
        nullptr,
    std::optional<std::map<SourceCallOccurrenceKey, va_t>> *ConstantTargets =
        nullptr);

/// After physical byte qualification, project FR=0 word FPRs into the paired
/// eight-byte ABI carriers before SSA. Preserve unbound bytes as unpredictable.
void projectMIPSFP32Registers(MedFunc &Function,
                              llvm::function_ref<MedVar(uint16_t)> Temporary,
                              llvm::function_ref<MedVar(unsigned)> Pair);

/// Match the original instruction, sequence and direct/indirect target.
const SourceCallTypeHint *mipsFloatingCallSite(
    const LowOp &Op, Arch Target,
    const std::map<SourceCallOccurrenceKey, SourceCallTypeHint> *Sites);

/// A scalar source result observes only its ABI width. Canonicalize an
/// unpredictable value only after proving every transported byte unobserved;
/// live unpredictable bytes retain their diagnostic intrinsic.
void discardUnobservedMIPSFPUpperBits(
    MedFunc &Function, const BinaryImage &Image,
    const std::map<va_t, FloatingCallArguments> &EntrySignatures,
    const DebugContext *Debug);

} // namespace neverd

#endif // NEVERD_IR_MED_MIPS_MIPSSOURCEVECTORS_H
