//===- RecoveryArchitecture.cpp - Native policy dispatch ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Explicit native recovery dispatch.

#include "../core/RecoveryArchitecture.h"

#include "x86_64/X64Recovery.h"

namespace neverd::analysis {
llvm::Error validateRuntimeMemoryOptions(const SpecializationOptions &Options) {
  if (Options.Architecture == Arch::X64)
    return x64::validateRuntimeMemoryOptions(Options);
  if (Options.X64SegmentedReads || Options.X64SegmentedWrites)
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "segmented memory options require the x64 architecture");
  return llvm::Error::success();
}

bool supportsRuntimeMemoryOperation(const LowOp &Op,
                                    const SpecializationOptions &Options) {
  return Options.Architecture == Arch::X64 &&
         x64::supportsRuntimeMemoryOperation(Op, Options);
}

void seedRuntimeMemoryState(symbolic::SymContext &Ctx,
                            symbolic::SymState &State,
                            const SpecializationOptions &Options) {
  if (Options.Architecture == Arch::X64)
    x64::seedRuntimeMemoryState(Ctx, State, Options);
}

llvm::Error
validateRuntimeControlOptions(const SpecializationOptions &Options) {
  if (Options.Architecture == Arch::X64)
    return x64::validateRuntimeControlOptions(Options);
  if (Options.X64CR4PGEState)
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "CR4 PGE state requires the x64 architecture");
  if (Options.X64MXCSRMask)
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "MXCSR state requires the x64 architecture");
  if (Options.X64LegacyGSState)
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "legacy GS state requires the x64 architecture");
  return llvm::Error::success();
}

bool supportsRuntimeControlOperation(const LowOp &Op,
                                     const SpecializationOptions &Options) {
  return Options.Architecture == Arch::X64 &&
         x64::supportsRuntimeControlOperation(Op, Options);
}

RuntimeControlState
seedRuntimeControlState(symbolic::SymContext &Ctx,
                        const SpecializationOptions &Options) {
  return Options.Architecture == Arch::X64
             ? x64::seedRuntimeControlState(Ctx, Options)
             : RuntimeControlState{};
}

RuntimeControlState
seedRuntimeControlState(symbolic::SymContext &Ctx, symbolic::SymState &State,
                        const SpecializationOptions &Options) {
  return Options.Architecture == Arch::X64
             ? x64::seedRuntimeControlState(Ctx, State, Options)
             : RuntimeControlState{};
}

llvm::Error commitRuntimeControlState(symbolic::SymState &State,
                                      RuntimeControlState &Controls,
                                      size_t Index, symbolic::SymRef Value,
                                      const SpecializationOptions &Options) {
  if (Options.Architecture == Arch::X64)
    return x64::commitRuntimeControlState(State, Controls, Index, Value,
                                          Options);
  return llvm::createStringError(llvm::errc::not_supported,
                                 "architecture has no runtime control owner");
}

std::optional<RuntimeControlTransition>
lowerRuntimeControlOperation(const LowOp &Op, symbolic::SymState &State,
                             const RuntimeControlState &Controls,
                             const SpecializationOptions &Options,
                             llvm::function_ref<NdVar(uint16_t)> Temporary) {
  if (Options.Architecture != Arch::X64)
    return std::nullopt;
  return x64::lowerRuntimeControlOperation(Op, State, Controls, Options,
                                           Temporary);
}

std::optional<RuntimeRegisterRead>
lowerRuntimeRegisterRead(const SpecializationInstruction &Instruction,
                         const SpecializationOptions &Options, NdVar Sample) {
  if (Options.Architecture != Arch::X64 || !Options.ExplicitMachineState ||
      !Options.NormalNonfaultingExecution ||
      Options.ByteOrder != llvm::endianness::little)
    return std::nullopt;
  return x64::lowerRuntimeRegisterRead(Instruction, Sample,
                                       Options.X64FlagsProfile);
}

llvm::Expected<const RecoveryArchitecture *> recoveryArchitecture(Arch Target) {
  switch (Target) {
  case Arch::X64:
    return &x64::recoveryArchitecture();
  default:
    return llvm::createStringError(
        llvm::errc::not_supported,
        "interpreter recovery architecture %u has no adapter",
        static_cast<unsigned>(Target));
  }
}

llvm::Error
validateSpecializationExternalExits(const SpecializationResult &Result,
                                    const SpecializationOptions &Options) {
  auto Architecture = recoveryArchitecture(Options.Architecture);
  if (!Architecture)
    return Architecture.takeError();
  return (*Architecture)->validateExternalExits(Result, Options);
}
} // namespace neverd::analysis
