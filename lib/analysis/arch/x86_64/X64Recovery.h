//===- X64Recovery.h - Native x64 recovery contracts -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_ANALYSIS_ARCH_X86_64_X64RECOVERY_H
#define NEVERD_ANALYSIS_ARCH_X86_64_X64RECOVERY_H

#include "../../core/RecoveryArchitecture.h"

#include "neverd/lift/X86/X86Regs.h"

#include <array>

namespace neverd::analysis::x64 {

inline constexpr uint64_t StackPointer = x86reg::RSP;
inline constexpr uint64_t ModelledFlags[] = {x86reg::CF, x86reg::PF, x86reg::AF,
                                             x86reg::ZF, x86reg::SF, x86reg::DF,
                                             x86reg::OF};

const RecoveryArchitecture &recoveryArchitecture();
llvm::Error validateRuntimeMemoryOptions(const SpecializationOptions &Options);
bool supportsRuntimeMemoryOperation(const LowOp &Op,
                                    const SpecializationOptions &Options);
void seedRuntimeMemoryState(symbolic::SymContext &Ctx,
                            symbolic::SymState &State,
                            const SpecializationOptions &Options);
llvm::Error validateRuntimeControlOptions(const SpecializationOptions &Options);
bool supportsRuntimeControlOperation(const LowOp &Op,
                                     const SpecializationOptions &Options);
RuntimeControlState
seedRuntimeControlState(symbolic::SymContext &Ctx,
                        const SpecializationOptions &Options);
RuntimeControlState
seedRuntimeControlState(symbolic::SymContext &Ctx, symbolic::SymState &State,
                        const SpecializationOptions &Options);
llvm::Error commitRuntimeControlState(symbolic::SymState &State,
                                      RuntimeControlState &Controls,
                                      size_t Index, symbolic::SymRef Value,
                                      const SpecializationOptions &Options);
std::optional<RuntimeControlTransition>
lowerRuntimeControlOperation(const LowOp &Op, symbolic::SymState &State,
                             const RuntimeControlState &Controls,
                             const SpecializationOptions &Options,
                             llvm::function_ref<NdVar(uint16_t)> Temporary);
std::string externalExitDigest(const SpecializationExternalExit &Exit);
llvm::Error validateExternalExits(const SpecializationResult &Result,
                                  const SpecializationOptions &Options);

/// Versioned conservative preparation reservation, validated before payload
/// inspection/copy/hash. Includes all options and mixed terminal declarations.
llvm::Expected<RawEnvironmentPreparation>
validateRawEnvironmentDeclarations(const SpecializationOptions &Options);

bool runtimeFlagsIntrinsic(
    const LowOp &Op,
    std::optional<InterpreterMachineStateProfile> Profile = std::nullopt);

struct RuntimeTimestampRead {
  std::array<LowOp, 3> Ops;
  std::array<NdVar, 2> PlaceholderOutputs;
};

/// Authenticate a complete RDTSC/RDTSCP native bundle before retaining its
/// host execution. Returns the full-width architectural output registers in
/// placeholder order. This does not grant host timestamp/processor identity.
std::optional<llvm::ArrayRef<uint64_t>>
runtimeTimestampRegisters(const SpecializationInstruction &Instruction,
                          NdVar Scratch);

/// Match the complete canonical x64 RDTSC bundle and bind one caller-supplied
/// sample to its primary result and two instruction-local output placeholders.
/// The original operation remains a runtime read. This scalar transition gives
/// no timestamp, privilege, nonfaulting, source or native-proof certificate.
std::optional<RuntimeTimestampRead>
lowerRuntimeTimestampRead(const SpecializationInstruction &Instruction,
                          NdVar Sample);

/// Authenticate complete canonical RDTSC, CPUID and supervisor RDMSR bundles.
/// CPUID retains its EAX/ECX selectors and four independent 32-bit answers.
/// RDMSR requires an explicit kernel profile and retains its actual ECX
/// selector. No host CPU value, repeated-query correlation or hardware
/// capability is inferred. Sample reserves sixteen bytes; the result states the
/// used width.
std::optional<RuntimeRegisterRead> lowerRuntimeRegisterRead(
    const SpecializationInstruction &Instruction, NdVar Sample,
    std::optional<InterpreterMachineStateProfile> Profile = std::nullopt);

/// Describe a complete canonical CPUID LowIR bundle. Candidate selectors may
/// be folded scalars; this validates no native bytes or execution domain.
std::optional<SpecializationScalarQuery>
describeCPUQuery(llvm::ArrayRef<LowOp> Operations);

/// Architectural result order shared by query descriptors and source adapters.
llvm::ArrayRef<uint64_t> cpuQueryResultRegisters();

bool isRetainedNativeTrap(const SpecializationInstruction &Insn);
bool isCetDisabledProjection(const SpecializationInstruction &Insn);

/// Reset and bind a separate bank fact only for the exact disabled-CET RDSSP
/// projection accepted by the owner above. Never upgrades undefined coverage.
bool bindCetDisabledPreservedState(SpecializationInstruction &Insn);

} // namespace neverd::analysis::x64

#endif
