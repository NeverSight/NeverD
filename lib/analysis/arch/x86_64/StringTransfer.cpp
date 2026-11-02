//===- StringTransfer.cpp - Bounded repeated memory transfers -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Scalar expansion preserving element order and runtime direction.

#include "StringTransfer.h"

#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/Support/Errc.h"

#include <algorithm>
#include <iterator>

namespace neverd::analysis::detail {
namespace {

bool value(const NdVar &V, uint16_t Size) {
  return V.Size == Size &&
         (V.isConst() ||
          ((V.isReg() || V.isTemp()) && V.Offset <= InvalidVA - (Size - 1)));
}

llvm::Error invalid(const char *Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, "%s", Message);
}

} // namespace

std::optional<StringTransferShape> stringTransferShape(const LowOp &Op) {
  if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs == 6 &&
      Op.Inputs[0].isConst()) {
    const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
    if ((Id != Intrinsic::X86RepMovsState &&
         Id != Intrinsic::X86RepStosState) ||
        !Op.Output.isTemp() ||
        !x86RepShapeIsValid(Id, x86RepLowShape(Op, Arch::X64)))
      return std::nullopt;
    const auto Control = x86RepControl(Id, Arch::X64, Op.Inputs[5].Offset);
    if (!Control || Control->AddressBytes != 8 || Control->Segment)
      return std::nullopt;
    for (unsigned I = 1; I < Op.NumInputs; ++I)
      if (!value(Op.Inputs[I], Op.Inputs[I].Size))
        return std::nullopt;
    if (!value(Op.Output, 8))
      return std::nullopt;
    const bool Fill = Id == Intrinsic::X86RepStosState;
    return StringTransferShape{Control->ElementBytes, Fill ? 2u : 3u, Fill,
                               true};
  }
  if (Op.Opcode != NdOp::INTRINSIC || Op.NumInputs != 5 ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 2 ||
      !Op.Output.isTemp() || !value(Op.Output, 8) || !value(Op.Inputs[4], 1))
    return std::nullopt;
  StringTransferShape Shape{};
  switch (static_cast<Intrinsic>(Op.Inputs[0].Offset)) {
  case Intrinsic::Movsb:
    Shape = {1, 3, false};
    break;
  case Intrinsic::Movsw:
    Shape = {2, 3, false};
    break;
  case Intrinsic::Movsd:
    Shape = {4, 3, false};
    break;
  case Intrinsic::Movsq:
    Shape = {8, 3, false};
    break;
  case Intrinsic::Stosb:
    Shape = {1, 2, true};
    break;
  case Intrinsic::Stosw:
    Shape = {2, 2, true};
    break;
  case Intrinsic::Stosd:
    Shape = {4, 2, true};
    break;
  case Intrinsic::Stosq:
    Shape = {8, 2, true};
    break;
  default:
    return std::nullopt;
  }
  if (!value(Op.Inputs[1], 8) || !value(Op.Inputs[2], 8) ||
      !value(Op.Inputs[3], Shape.Fill ? Shape.ElementBytes : 8))
    return std::nullopt;
  return Shape;
}

llvm::Expected<std::vector<LowOp>>
lowerStringTransfer(const LowOp &Op, uint64_t Count,
                    std::optional<bool> Backward,
                    llvm::ArrayRef<LowOp> InstructionOps,
                    uint64_t TemporaryLimit, uint64_t MaxOperations) {
  const auto Shape = stringTransferShape(Op);
  if (!Shape)
    return invalid("unsupported repeated memory transfer shape");
  std::vector<LowOp> Result;
  const bool RuntimeDirection = !Backward && Count;
  const uint64_t PerElement = Shape->elementOperations(RuntimeDirection);
  const uint64_t Completion = Shape->fixedOperations(Count, RuntimeDirection);
  if (MaxOperations < Completion ||
      Count > (MaxOperations - Completion) / PerElement ||
      Count > (Result.max_size() - Completion) / PerElement)
    return invalid("repeated memory transfer exceeds operation budget");

  uint64_t Next = 0;
  const auto Scan = [&](const NdVar &V) {
    if (!V.isTemp() || !V.Size)
      return true;
    if (V.Offset >= TemporaryLimit || V.Size > TemporaryLimit - V.Offset)
      return false;
    Next = std::max(Next, V.Offset + V.Size);
    return true;
  };
  if (!Scan(Op.Output))
    return invalid("repeated memory transfer temporary is out of range");
  for (const auto &Original : InstructionOps) {
    if (Original.NumInputs > std::size(Original.Inputs) ||
        !Scan(Original.Output))
      return invalid("invalid repeated memory transfer instruction operands");
    for (unsigned I = 0; I < Original.NumInputs; ++I)
      if (!Scan(Original.Inputs[I]))
        return invalid("repeated memory transfer temporary is out of range");
  }
  for (unsigned I = 0; I < Op.NumInputs; ++I)
    if (!Scan(Op.Inputs[I]))
      return invalid("repeated memory transfer temporary is out of range");
  const unsigned ScratchBytes = RuntimeDirection ? 40 : 24;
  if (Count && (Next > TemporaryLimit || TemporaryLimit - Next < ScratchBytes))
    return invalid("repeated memory transfer has no disjoint scratch range");

  std::vector<NdVar> Outputs{Op.Output};
  if (Shape->CompleteState) {
    // The Low-to-Med convention binds auxiliary definitions through the
    // immediately following temporary copies. Require the exact sequence
    // before expanding it; an unbound or aliased output must fail clearly.
    size_t Position = InstructionOps.size();
    for (size_t I = 0; I < InstructionOps.size(); ++I)
      if (InstructionOps[I].Opcode == Op.Opcode &&
          InstructionOps[I].Addr == Op.Addr &&
          InstructionOps[I].Seq == Op.Seq &&
          InstructionOps[I].Output == Op.Output) {
        if (Position != InstructionOps.size())
          return invalid("ambiguous repeated transfer completion");
        Position = I;
      }
    const unsigned Words = Shape->Fill ? 2 : 3;
    if (Position == InstructionOps.size() ||
        InstructionOps.size() - Position < Words)
      return invalid("missing repeated transfer completion");
    for (unsigned I = 1; I < Words; ++I) {
      const auto &Bind = InstructionOps[Position + I];
      if (Bind.Addr != Op.Addr || Bind.Opcode != NdOp::COPY ||
          Bind.NumInputs != 1 || !Bind.Output.isTemp() ||
          !value(Bind.Output, 8) || !Bind.Inputs[0].isTemp() ||
          !value(Bind.Inputs[0], 8) ||
          Bind.MemoryOrdering != NdMemoryOrdering::None ||
          Bind.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        return invalid("invalid repeated transfer completion binding");
      Outputs.push_back(Bind.Inputs[0]);
    }
    const auto Overlaps = [](const NdVar &A, const NdVar &B) {
      return A.isTemp() && B.isTemp() && A.Size && B.Size &&
             (A.Offset <= B.Offset ? B.Offset - A.Offset < A.Size
                                   : A.Offset - B.Offset < B.Size);
    };
    for (unsigned I = 0; I < Outputs.size(); ++I) {
      for (unsigned J = 0; J < I; ++J)
        if (Overlaps(Outputs[I], Outputs[J]))
          return invalid("aliased repeated transfer completion");
      for (size_t J = 0; J < InstructionOps.size(); ++J) {
        const auto &Other = InstructionOps[J];
        if (!(I == 0 && J == Position) && Overlaps(Outputs[I], Other.Output))
          return invalid("redefined repeated transfer completion");
        if (J <= Position)
          for (unsigned K = 0; K < Other.NumInputs; ++K)
            if (Overlaps(Outputs[I], Other.Inputs[K]))
              return invalid("repeated transfer completion aliases input");
      }
    }
  }

  Result.reserve(static_cast<size_t>(Count * PerElement + Completion));
  const auto Emit = [&](NdOp Code, NdVar Output,
                        std::initializer_list<NdVar> Inputs) {
    LowOp New;
    New.Opcode = Code;
    New.Addr = Op.Addr;
    New.Seq = Op.Seq;
    New.Output = Output;
    for (const auto &Input : Inputs)
      New.addInput(Input);
    Result.push_back(New);
  };
  const NdVar SourceAddress = NdVar::tmp(Next, 8);
  const NdVar DestinationAddress = NdVar::tmp(Next + 8, 8);
  const NdVar Element = NdVar::tmp(Next + 16, Shape->ElementBytes);
  const NdVar DirectionMask = NdVar::tmp(Next + 24, Op.Inputs[4].Size);
  const NdVar Direction = NdVar::tmp(Next + 28, 1);
  const NdVar SelectedDelta = NdVar::tmp(Next + 32, 8);
  if (RuntimeDirection) {
    NdVar Input = Op.Inputs[4];
    if (Shape->CompleteState) {
      Emit(NdOp::INT_AND, DirectionMask,
           {Input, NdVar::scalar(1u << X86RepDirectionShift, Input.Size)});
      Input = DirectionMask;
    }
    Emit(NdOp::INT_NOTEQUAL, Direction, {Input, NdVar::scalar(0, Input.Size)});
  }
  const auto DeltaFor = [&](uint64_t Distance) {
    if (!RuntimeDirection)
      return NdVar::scalar(
          Backward.value_or(false) ? uint64_t{0} - Distance : Distance, 8);
    Emit(NdOp::SELECT, SelectedDelta,
         {Direction, NdVar::scalar(uint64_t{0} - Distance, 8),
          NdVar::scalar(Distance, 8)});
    return SelectedDelta;
  };
  const bool TruncateFill =
      Shape->CompleteState && Shape->Fill && Shape->ElementBytes != 8;
  if (Count && TruncateFill)
    Emit(NdOp::SUBBYTES, Element, {Op.Inputs[3], NdVar::scalar(0, 4)});
  for (uint64_t I = 0; I < Count; ++I) {
    const NdVar Delta = DeltaFor(I * Shape->ElementBytes);
    if (!Shape->Fill) {
      Emit(NdOp::INT_ADD, SourceAddress, {Op.Inputs[1], Delta});
      Emit(NdOp::LOAD, Element, {SourceAddress});
    }
    Emit(NdOp::INT_ADD, DestinationAddress,
         {Op.Inputs[Shape->Fill ? 1 : 2], Delta});
    Emit(NdOp::STORE, {},
         {DestinationAddress,
          Shape->Fill && !TruncateFill ? Op.Inputs[3] : Element});
  }
  if (Shape->CompleteState) {
    const auto Delta = DeltaFor(Count * Shape->ElementBytes);
    Emit(NdOp::INT_ADD, Outputs[0], {Op.Inputs[1], Delta});
    if (!Shape->Fill)
      Emit(NdOp::INT_ADD, Outputs[1], {Op.Inputs[2], Delta});
    Emit(NdOp::COPY, Outputs.back(), {NdVar::scalar(0, 8)});
  } else
    Emit(NdOp::COPY, Op.Output, {NdVar::scalar(0, 8)});
  return Result;
}

} // namespace neverd::analysis::detail
