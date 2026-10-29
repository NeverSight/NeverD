//===- X64LegacyGS.cpp - Explicit legacy GS exchange --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exact GS.base/IA32_KERNEL_GS_BASE exchange in the declared legacy domain.
//===----------------------------------------------------------------------===//

#include "X64ControlTransitions.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/symbolic/SymState.h"

namespace neverd::analysis::x64::legacygs {
namespace {
bool validDomain(const SpecializationOptions &Options) {
  return Options.Architecture == Arch::X64 && Options.ExplicitMachineState &&
         Options.NormalNonfaultingExecution &&
         Options.ByteOrder == llvm::endianness::little &&
         Options.X64FlagsProfile &&
         isKernelX64MachineStateProfile(*Options.X64FlagsProfile);
}

bool operation(const LowOp &Op) {
  return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs == 1 && !Op.Output.Size &&
         Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
         Op.MemoryOrdering == NdMemoryOrdering::None &&
         Op.Inputs[0].isConst() && Op.Inputs[0].Size == 2 &&
         Op.Inputs[0].Offset == uint64_t(Intrinsic::Swapgs);
}

bool temporary(NdVar Value, uint16_t Bytes) {
  return Value.isTemp() && Value.Size == Bytes &&
         Value.Offset <= InvalidVA - (Bytes - 1);
}

bool overlaps(NdVar Left, NdVar Right) {
  return Left.Offset <= Right.Offset + Right.Size - 1 &&
         Right.Offset <= Left.Offset + Left.Size - 1;
}
} // namespace

llvm::Error validateOptions(const SpecializationOptions &Options) {
  if (Options.X64LegacyGSState && !validDomain(Options))
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "legacy GS state requires explicit nonfaulting little-endian x64 "
        "kernel state with CR4.FRED disabled");
  return llvm::Error::success();
}

bool supportsOperation(const LowOp &Op, const SpecializationOptions &Options) {
  return Options.X64LegacyGSState && validDomain(Options) && operation(Op);
}

std::optional<RuntimeControlTransition>
lowerScalarOperation(const LowOp &Op, NdVar Pair,
                     llvm::function_ref<NdVar(uint16_t)> Temporary) {
  if (!operation(Op))
    return std::nullopt;
  const auto Active = Temporary(8), Inactive = Temporary(8);
  if (!temporary(Pair, 16) || !temporary(Active, 8) ||
      !temporary(Inactive, 8) || overlaps(Pair, Active) ||
      overlaps(Pair, Inactive) || overlaps(Active, Inactive))
    return std::nullopt;

  RuntimeControlTransition Result;
  const auto Emit = [&](NdOp Code, NdVar Output,
                        std::initializer_list<NdVar> Inputs) {
    LowOp Scalar;
    Scalar.Opcode = Code;
    Scalar.Output = Output;
    Scalar.Addr = Op.Addr;
    Scalar.Seq = static_cast<int>(Result.Ops.size());
    for (const auto Input : Inputs)
      Scalar.addInput(Input);
    Result.Ops.push_back(Scalar);
  };
  // Intel SDM vol.2 SWAPGS: exchange both complete 64-bit bases, leaving
  // flags and all other registers unchanged. CPL=0 and CR4.FRED=0 are explicit
  // domain preconditions, not inferred from a successful observation. The low
  // half is always the active base used by the segmented-memory owner.
  Emit(NdOp::SUBBYTES, Active, {Pair, NdVar::scalar(0, 8)});
  Emit(NdOp::SUBBYTES, Inactive, {Pair, NdVar::scalar(8, 8)});
  Emit(NdOp::CONCAT, Pair, {Active, Inactive});
  Result.OutputState = std::pair<size_t, NdVar>{0, Pair};
  return Result;
}

std::optional<RuntimeControlTransition>
lowerOperation(const LowOp &Op, symbolic::SymState &State,
               const RuntimeControlState &Controls,
               const SpecializationOptions &Options,
               llvm::function_ref<NdVar(uint16_t)> Temporary) {
  if (!supportsOperation(Op, Options) ||
      !control::validState(State, Controls, Options))
    return std::nullopt;
  const auto Pair = Temporary(16);
  auto Result = lowerScalarOperation(Op, Pair, Temporary);
  if (!Result)
    return std::nullopt;
  const size_t Index = Options.X64MXCSRMask.has_value();
  State.write(symbolic::SymSpace::Temporary, Pair.Offset,
              Controls.Values[Index]);
  Result->OutputState->first = Index;
  return Result;
}

} // namespace neverd::analysis::x64::legacygs
