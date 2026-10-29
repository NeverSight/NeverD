//===- X64ControlTransitions.h - Checked control owners --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Private owners of checked x64 runtime control transitions.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_ARCH_X86_64_X64CONTROLTRANSITIONS_H
#define NEVERD_ANALYSIS_ARCH_X86_64_X64CONTROLTRANSITIONS_H

#include "../../core/RecoveryArchitecture.h"

namespace neverd::analysis::x64::control {
size_t stateSize(const SpecializationOptions &Options);
bool validState(symbolic::SymState &State, const RuntimeControlState &Controls,
                const SpecializationOptions &Options);
} // namespace neverd::analysis::x64::control

namespace neverd::analysis::x64::legacygs {
llvm::Error validateOptions(const SpecializationOptions &Options);
bool supportsOperation(const LowOp &Op, const SpecializationOptions &Options);
/// Pure instruction transition on an already validated active/inactive pair.
/// The caller supplies the execution domain and owns state storage/commit.
std::optional<RuntimeControlTransition>
lowerScalarOperation(const LowOp &Op, NdVar Pair,
                     llvm::function_ref<NdVar(uint16_t)> Temporary);
std::optional<RuntimeControlTransition>
lowerOperation(const LowOp &Op, symbolic::SymState &State,
               const RuntimeControlState &Controls,
               const SpecializationOptions &Options,
               llvm::function_ref<NdVar(uint16_t)> Temporary);
} // namespace neverd::analysis::x64::legacygs

namespace neverd::analysis::x64::mxcsr {
llvm::Error validateOptions(const SpecializationOptions &Options);
bool supportsOperation(const LowOp &Op, const SpecializationOptions &Options);
/// Includes the memory store form. Native proof admission remains separate.
bool supportsScalarOperation(const LowOp &Op,
                             const SpecializationOptions &Options);
/// Shared scalar transition; Rejected must be checked before committing any
/// architectural effect. This function grants no execution-domain evidence.
std::optional<RuntimeControlTransition>
lowerScalarOperation(const LowOp &Op, NdVar Slot, uint32_t Mask,
                     llvm::function_ref<NdVar(uint16_t)> Temporary);
RuntimeControlState seedState(symbolic::SymContext &Ctx,
                              const SpecializationOptions &Options);
std::optional<RuntimeControlTransition>
lowerOperation(const LowOp &Op, symbolic::SymState &State,
               const RuntimeControlState &Controls,
               const SpecializationOptions &Options,
               llvm::function_ref<NdVar(uint16_t)> Temporary);
} // namespace neverd::analysis::x64::mxcsr

namespace neverd::analysis::x64::alignment {
bool supportsOperation(const LowOp &Op, const SpecializationOptions &Options);
std::optional<RuntimeControlTransition>
lowerOperation(const LowOp &Op, const SpecializationOptions &Options,
               llvm::function_ref<NdVar(uint16_t)> Temporary);
} // namespace neverd::analysis::x64::alignment

#endif
