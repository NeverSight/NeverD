//===- X64Recovery.h - Native x64 recovery contracts -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_ANALYSIS_ARCH_X86_64_X64RECOVERY_H
#define NEVERD_ANALYSIS_ARCH_X86_64_X64RECOVERY_H

#include "neverd/analysis/InterpreterSpecialization.h"
#include "neverd/lift/X86Regs.h"

namespace neverd::analysis::x64 {

inline constexpr uint64_t StackPointer = x86reg::RSP;
inline constexpr uint64_t ModelledFlags[] = {x86reg::CF, x86reg::PF, x86reg::AF,
                                             x86reg::ZF, x86reg::SF, x86reg::DF,
                                             x86reg::OF};

bool runtimeFlagsIntrinsic(const LowOp &Op);
bool isRetainedNativeTrap(const SpecializationInstruction &Insn);
bool isCetDisabledProjection(const SpecializationInstruction &Insn);

/// Reset and bind a separate bank fact only for the exact disabled-CET RDSSP
/// projection accepted by the owner above. Never upgrades undefined coverage.
bool bindCetDisabledPreservedState(SpecializationInstruction &Insn);

} // namespace neverd::analysis::x64

#endif
