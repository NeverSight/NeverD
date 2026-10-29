//===- X64MXCSR.cpp - Checked floating-point control transitions --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Architecture-owned MXCSR domains and checked scalar state transitions.
//===----------------------------------------------------------------------===//

#include "X64ControlTransitions.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/symbolic/SymState.h"

namespace neverd::analysis::x64::mxcsr {
namespace {
enum class Operation { Load, Store, Read, Write };

bool validMask(uint32_t Mask) { return Mask == 0xffbf || Mask == 0xffff; }

std::optional<Operation> operation(const LowOp &Op) {
  if (Op.Opcode != NdOp::INTRINSIC || Op.NumInputs == 0 ||
      !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 2 ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Op.MemoryOrdering != NdMemoryOrdering::None)
    return std::nullopt;
  const auto Scalar = [](NdVar V, uint16_t Bytes) {
    return V.Size == Bytes &&
           (V.isConst() ||
            ((V.isReg() || V.isTemp()) && V.Offset <= InvalidVA - (Bytes - 1)));
  };
  switch (static_cast<Intrinsic>(Op.Inputs[0].Offset)) {
  case Intrinsic::Ldmxcsr:
    if (Op.NumInputs == 2 && !Op.Output.Size && Scalar(Op.Inputs[1], 8))
      return Operation::Load;
    break;
  case Intrinsic::Stmxcsr:
    if (Op.NumInputs == 2 && !Op.Output.Size && Scalar(Op.Inputs[1], 8))
      return Operation::Store;
    break;
  case Intrinsic::X86ReadMXCSR:
    if (Op.NumInputs == 1 && !Op.Output.isConst() && Scalar(Op.Output, 4))
      return Operation::Read;
    break;
  case Intrinsic::X86WriteMXCSR:
    if (Op.NumInputs == 2 && !Op.Output.Size && Scalar(Op.Inputs[1], 4))
      return Operation::Write;
    break;
  default:
    break;
  }
  return std::nullopt;
}
} // namespace

llvm::Error validateOptions(const SpecializationOptions &Options) {
  if (!Options.X64MXCSRMask)
    return llvm::Error::success();
  if (!Options.ExplicitMachineState || !Options.NormalNonfaultingExecution)
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "MXCSR state requires explicit nonfaulting machine state");
  if (Options.ByteOrder != llvm::endianness::little)
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "MXCSR state requires x64 byte order");
  if (!validMask(*Options.X64MXCSRMask))
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "unsupported MXCSR writable-bit mask");
  return llvm::Error::success();
}

bool supportsOperation(const LowOp &Op, const SpecializationOptions &Options) {
  return supportsScalarOperation(Op, Options) &&
         operation(Op) != Operation::Store;
}

bool supportsScalarOperation(const LowOp &Op,
                             const SpecializationOptions &Options) {
  return Options.X64MXCSRMask.has_value() && operation(Op).has_value();
}

RuntimeControlState seedState(symbolic::SymContext &Ctx,
                              const SpecializationOptions &Options) {
  if (!Options.X64MXCSRMask)
    return {};
  // Intel SDM vol.1 11.6.6: MXCSR_MASK describes writable bits; a zero FXSAVE
  // mask falls back to 0xffbf. This API accepts an explicit effective mask,
  // not an observed address or an implicit FXSAVE/CPU feature certificate.
  return {{Ctx.mkAnd(Ctx.mkFreshVar(32, "mxcsr"),
                     Ctx.mkConst(32, *Options.X64MXCSRMask))}};
}

std::optional<RuntimeControlTransition>
lowerScalarOperation(const LowOp &Op, NdVar Slot, uint32_t Mask,
                     llvm::function_ref<NdVar(uint16_t)> Temporary) {
  const auto Kind = operation(Op);
  if (!Kind || !validMask(Mask) || !Slot.isTemp() || Slot.Size != 4 ||
      Slot.Offset > InvalidVA - 3)
    return std::nullopt;
  RuntimeControlTransition Result;
  Result.InvalidValueDiagnostic = "MXCSR write may set reserved bits";
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
  if (*Kind == Operation::Store) {
    Emit(NdOp::STORE, {}, {Op.Inputs[1], Slot});
    return Result;
  }
  NdVar Value = Op.Inputs[1];
  if (*Kind == Operation::Load) {
    Value = Temporary(4);
    Emit(NdOp::LOAD, Value, {Op.Inputs[1]});
  }
  const auto Reserved = Temporary(4), Bad = Temporary(1);
  Emit(NdOp::INT_AND, Reserved, {Value, NdVar::scalar(~Mask, 4)});
  Emit(NdOp::INT_NOTEQUAL, Bad, {Reserved, NdVar::scalar(0, 4)});
  // Intel SDM vol.2 LDMXCSR: reserved-bit writes fault before changing state.
  // No exception is silently discarded: the consumer must prove Bad == 0
  // before committing this transition or publishing its retained intrinsic.
  Emit(NdOp::SELECT, Slot, {Bad, Slot, Value});
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
  const auto Slot = Temporary(4);
  auto Result =
      lowerScalarOperation(Op, Slot, *Options.X64MXCSRMask, Temporary);
  if (!Result)
    return std::nullopt;
  State.write(symbolic::SymSpace::Temporary, Slot.Offset, Controls.Values[0]);
  return Result;
}

} // namespace neverd::analysis::x64::mxcsr
