//===- X64RuntimeControl.cpp - Checked x64 control dispatch -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Dispatch runtime control operations without coupling independent owners.
//===----------------------------------------------------------------------===//

#include "X64ControlTransitions.h"
#include "X64Recovery.h"
#include "X64StreamStore.h"

#include "neverd/symbolic/SymState.h"

namespace neverd::analysis::x64 {
namespace control {
size_t stateSize(const SpecializationOptions &Options) {
  return size_t(Options.X64MXCSRMask.has_value()) + Options.X64LegacyGSState +
         Options.X64CR4PGEState;
}

bool validState(symbolic::SymState &State, const RuntimeControlState &Controls,
                const SpecializationOptions &Options) {
  auto &Ctx = State.context();
  const auto HasWidth = [&](symbolic::SymRef Value, unsigned Width) {
    return Value && Value.index() < Ctx.numNodes() && Ctx.width(Value) == Width;
  };
  if (Controls.Values.size() != stateSize(Options))
    return false;
  if (Options.X64MXCSRMask && !HasWidth(Controls.Values[0], 32))
    return false;
  if (Options.X64LegacyGSState) {
    const auto Pair = Controls.Values[Options.X64MXCSRMask.has_value()];
    const auto Active =
        State.x64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS);
    if (!HasWidth(Pair, 128) || !HasWidth(Active, 64) ||
        Ctx.mkExtract(Pair, 0, 64) != Active)
      return false;
  }
  if (Options.X64CR4PGEState && !HasWidth(Controls.Values.back(), 64))
    return false;
  return true;
}
} // namespace control

llvm::Error
validateRuntimeControlOptions(const SpecializationOptions &Options) {
  if (auto Error = legacygs::validateOptions(Options))
    return Error;
  if (auto Error = pageglobal::validateOptions(Options))
    return Error;
  return mxcsr::validateOptions(Options);
}

bool supportsRuntimeControlOperation(const LowOp &Op,
                                     const SpecializationOptions &Options) {
  return mxcsr::supportsOperation(Op, Options) ||
         alignment::supportsOperation(Op, Options) ||
         readprefetch::supportsOperation(Op, Options) ||
         streamstore::supportsFence(Op, Options) ||
         legacygs::supportsOperation(Op, Options) ||
         pageglobal::supportsOperation(Op, Options);
}

RuntimeControlState
seedRuntimeControlState(symbolic::SymContext &Ctx,
                        const SpecializationOptions &Options) {
  // A context alone cannot supply the active memory base. Keep old MXCSR-only
  // callers working, but never invent an unrelated GS input for them.
  if (Options.X64LegacyGSState)
    return {};
  auto Result = mxcsr::seedState(Ctx, Options);
  if (Options.X64CR4PGEState)
    Result.Values.push_back(Ctx.mkFreshVar(64, "cr4"));
  return Result;
}

RuntimeControlState
seedRuntimeControlState(symbolic::SymContext &Ctx, symbolic::SymState &State,
                        const SpecializationOptions &Options) {
  if (Options.X64CR4PGEState && &Ctx != &State.context())
    return {};
  auto Result = mxcsr::seedState(Ctx, Options);
  if (Options.X64LegacyGSState) {
    const auto Active =
        State.x64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS);
    if (&Ctx != &State.context() || !Active || Ctx.width(Active) != 64)
      return {};
    Result.Values.push_back(
        Ctx.mkConcat(Ctx.mkFreshVar(64, "kernel_gs_base"), Active));
  }
  if (Options.X64CR4PGEState)
    Result.Values.push_back(Ctx.mkFreshVar(64, "cr4"));
  return Result;
}

llvm::Error commitRuntimeControlState(symbolic::SymState &State,
                                      RuntimeControlState &Controls,
                                      size_t Index, symbolic::SymRef Value,
                                      const SpecializationOptions &Options) {
  auto &Ctx = State.context();
  if (!control::validState(State, Controls, Options) ||
      Index >= Controls.Values.size() || !Value ||
      Value.index() >= Ctx.numNodes() ||
      Ctx.width(Value) != Ctx.width(Controls.Values[Index]))
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "invalid runtime control state update");
  if (Options.X64LegacyGSState &&
      Index == size_t(Options.X64MXCSRMask.has_value())) {
    const auto Active = Ctx.mkExtract(Value, 0, 64);
    if (!State.setX64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS,
                                            Active))
      return llvm::createStringError(llvm::errc::invalid_argument,
                                     "invalid active GS base update");
  }
  Controls.Values[Index] = Value;
  return llvm::Error::success();
}

std::optional<RuntimeControlTransition>
lowerRuntimeControlOperation(const LowOp &Op, symbolic::SymState &State,
                             const RuntimeControlState &Controls,
                             const SpecializationOptions &Options,
                             llvm::function_ref<NdVar(uint16_t)> Temporary) {
  // PREFETCHh is a retained cache hint, not a guest RAM read or a scalar
  // state write. The original intrinsic still executes in residual/source.
  if (readprefetch::supportsOperation(Op, Options))
    return RuntimeControlTransition{};
  if (streamstore::supportsFence(Op, Options))
    return RuntimeControlTransition{};
  if (alignment::supportsOperation(Op, Options))
    return alignment::lowerOperation(Op, Options, Temporary);
  if (legacygs::supportsOperation(Op, Options))
    return legacygs::lowerOperation(Op, State, Controls, Options, Temporary);
  if (pageglobal::supportsOperation(Op, Options))
    return pageglobal::lowerOperation(Op, State, Controls, Options, Temporary);
  return mxcsr::lowerOperation(Op, State, Controls, Options, Temporary);
}
} // namespace neverd::analysis::x64
