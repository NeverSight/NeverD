//===- X64PageGlobal.cpp - Bounded CR4 value transitions -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Retained control transfers under an explicit stable-translation domain.
//===----------------------------------------------------------------------===//

#include "X64ControlTransitions.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/symbolic/SymState.h"

namespace neverd::analysis::x64::pageglobal {
namespace {
enum class Operation { Read, Write };

bool validDomain(const SpecializationOptions &Options) {
  return Options.Architecture == Arch::X64 && Options.ExplicitMachineState &&
         Options.NormalNonfaultingExecution &&
         Options.ByteOrder == llvm::endianness::little &&
         Options.X64FlagsProfile &&
         isKernelX64MachineStateProfile(*Options.X64FlagsProfile);
}

std::optional<Operation> operation(const LowOp &Op) {
  if (Op.Opcode != NdOp::INTRINSIC || !Op.NumInputs ||
      !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 2 ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Op.MemoryOrdering != NdMemoryOrdering::None)
    return std::nullopt;
  const auto Scalar = [](NdVar Value) {
    return Value.Size == 8 &&
           (Value.isConst() || ((Value.isReg() || Value.isTemp()) &&
                                Value.Offset <= InvalidVA - 7));
  };
  if (Op.Inputs[0].Offset == static_cast<uint64_t>(Intrinsic::ReadCr4) &&
      Op.NumInputs == 1 && !Op.Output.isConst() && Scalar(Op.Output))
    return Operation::Read;
  if (Op.Inputs[0].Offset == static_cast<uint64_t>(Intrinsic::WriteCr4) &&
      Op.NumInputs == 2 && !Op.Output.Size && Scalar(Op.Inputs[1]))
    return Operation::Write;
  return std::nullopt;
}
} // namespace

llvm::Error validateOptions(const SpecializationOptions &Options) {
  if (Options.X64CR4PGEState && !validDomain(Options))
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "CR4 PGE state requires explicit nonfaulting little-endian x64 "
        "kernel machine state");
  return llvm::Error::success();
}

bool supportsOperation(const LowOp &Op, const SpecializationOptions &Options) {
  return Options.X64CR4PGEState && validDomain(Options) &&
         operation(Op).has_value();
}

std::optional<RuntimeControlTransition>
lowerScalarOperation(const LowOp &Op, NdVar Slot,
                     llvm::function_ref<NdVar(uint16_t)> Temporary) {
  const auto Kind = operation(Op);
  if (!Kind || !Slot.isTemp() || Slot.Size != 8 || Slot.Offset > InvalidVA - 7)
    return std::nullopt;
  RuntimeControlTransition Result;
  Result.InvalidValueDiagnostic = "CR4 write may change a non-PGE control bit";
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
  if (*Kind == Operation::Read) {
    Emit(NdOp::COPY, Op.Output, {Slot});
    return Result;
  }
  // Intel SDM Vol.3A, CR4 and 4.10.4.1: PGE is bit 7; changing it can
  // invalidate translations. The declared stable-translation domain excludes
  // observations of that effect. Keep the original serializing MOV in LowIR;
  // this pure transition supplies only its scalar value relationship.
  constexpr uint64_t PGE = uint64_t{1} << 7;
  const auto Changed = Temporary(8), Other = Temporary(8);
  const auto Bad = Temporary(1);
  Emit(NdOp::INT_XOR, Changed, {Slot, Op.Inputs[1]});
  Emit(NdOp::INT_AND, Other, {Changed, NdVar::scalar(~PGE, 8)});
  Emit(NdOp::INT_NOTEQUAL, Bad, {Other, NdVar::scalar(0, 8)});
  Emit(NdOp::SELECT, Slot, {Bad, Slot, Op.Inputs[1]});
  Result.Rejected = Bad;
  Result.OutputState = std::pair<size_t, NdVar>{0, Slot};
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
  const size_t Index =
      size_t(Options.X64MXCSRMask.has_value()) + Options.X64LegacyGSState;
  const auto Slot = Temporary(8);
  auto Result = lowerScalarOperation(Op, Slot, Temporary);
  if (!Result)
    return std::nullopt;
  State.write(symbolic::SymSpace::Temporary, Slot.Offset,
              Controls.Values[Index]);
  if (Result->OutputState)
    Result->OutputState->first = Index;
  return Result;
}
} // namespace neverd::analysis::x64::pageglobal
