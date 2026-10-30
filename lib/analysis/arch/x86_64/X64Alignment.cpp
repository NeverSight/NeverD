//===- X64Alignment.cpp - Checked address-alignment transitions ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exact default-space alignment predicates for explicit x64 recovery.
//===----------------------------------------------------------------------===//

#include "X64ControlTransitions.h"

#include "neverd/ir/intrinsics/Intrinsics.h"

namespace neverd::analysis::x64::alignment {
namespace {
bool scalar(NdVar Value, uint16_t Bytes) {
  return Value.Size == Bytes &&
         (Value.isConst() || ((Value.isReg() || Value.isTemp()) &&
                              Value.Offset <= InvalidVA - (Bytes - 1)));
}

bool overlaps(NdVar Left, NdVar Right) {
  if (!Left.isTemp() || !Right.isTemp())
    return false;
  // The caller has already validated each inclusive extent against overflow.
  return Left.Offset <= Right.Offset + Right.Size - 1 &&
         Right.Offset <= Left.Offset + Left.Size - 1;
}
} // namespace

bool supportsOperation(const LowOp &Op, const SpecializationOptions &Options) {
  if (Options.Architecture != Arch::X64 || !Options.ExplicitMachineState ||
      !Options.NormalNonfaultingExecution ||
      Options.ByteOrder != llvm::endianness::little ||
      Op.Opcode != NdOp::INTRINSIC || Op.NumInputs != 3 || Op.Output.Size ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 2 ||
      Op.Inputs[0].Offset != uint64_t(Intrinsic::RequireAligned) ||
      !scalar(Op.Inputs[1], 8) || !Op.Inputs[2].isConst() ||
      Op.Inputs[2].Size != 8)
    return false;
  const uint64_t Alignment = Op.Inputs[2].Offset;
  return Alignment && (Alignment & (Alignment - 1)) == 0;
}

std::optional<RuntimeControlTransition>
lowerOperation(const LowOp &Op, const SpecializationOptions &Options,
               llvm::function_ref<NdVar(uint16_t)> Temporary) {
  if (!supportsOperation(Op, Options))
    return std::nullopt;
  const auto LowBits = Temporary(8), Bad = Temporary(1);
  if (!LowBits.isTemp() || !scalar(LowBits, 8) || !Bad.isTemp() ||
      !scalar(Bad, 1) || overlaps(LowBits, Bad) ||
      overlaps(LowBits, Op.Inputs[1]) || overlaps(Bad, Op.Inputs[1]))
    return std::nullopt;

  RuntimeControlTransition Result;
  Result.InvalidValueDiagnostic = "memory address may violate alignment";
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
  Emit(NdOp::INT_AND, LowBits,
       {Op.Inputs[1], NdVar::scalar(Op.Inputs[2].Offset - 1, 8)});
  Emit(NdOp::INT_NOTEQUAL, Bad, {LowBits, NdVar::scalar(0, 8)});
  // Explicit nonfaulting execution is not an alignment proof. The common
  // consumer must prove this predicate is always zero before retaining the
  // original intrinsic and proceeding to the later memory operation.
  Result.Rejected = Bad;
  return Result;
}

std::optional<bool> constantValidity(const LowOp &Op,
                                     const SpecializationOptions &Options) {
  if (!supportsOperation(Op, Options) || !Op.Inputs[1].isConst())
    return std::nullopt;
  return (Op.Inputs[1].Offset & (Op.Inputs[2].Offset - 1)) == 0;
}
} // namespace neverd::analysis::x64::alignment
