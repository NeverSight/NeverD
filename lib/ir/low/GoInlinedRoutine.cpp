//===- GoInlinedRoutine.cpp - Go routines a call runs in place ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Lifts the straight-line body of a Go routine that runs in its caller's
/// place, from an entry to its return.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/GoInlinedRoutine.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/GoCallContract.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

namespace neverd {

namespace {

/// An operation the routine's body cannot hold: one that leaves it, whose
/// effects no operation of the lift states, or that synchronizes memory.
bool leavesStraightLineBody(const LowOp &Op) {
  switch (Op.Opcode) {
  case NdOp::BRANCH:
  case NdOp::COND_BR:
  case NdOp::INDIR_BR:
  case NdOp::CALL:
  case NdOp::INDIR_CALL:
  case NdOp::INTRINSIC:
  case NdOp::ATOMIC_XCHG:
  case NdOp::ATOMIC_ADD:
  case NdOp::ATOMIC_CMPXCHG:
    return true;
  default:
    return Op.MemoryOrdering != NdMemoryOrdering::None;
  }
}

} // namespace

std::optional<std::vector<LowOp>> goInlinedRoutineBody(const BinaryImage &Img,
                                                       va_t Entry) {
  if (!goInlinedRoutineAt(Img, Entry))
    return std::nullopt;
  // A decoder of its own: the caller's still holds the call it decoded.
  Decoder Dec;
  if (!Dec.init(Img))
    return std::nullopt;
  const uint64_t StackPointer = getTargetRegInfo(Img.Arch).StackPointer;
  std::vector<LowOp> Body;
  // The routine's extent bounds the walk: each instruction lies inside it.
  for (va_t At = Entry; goInlinedRoutineAt(Img, At);) {
    const auto *Seg = Img.getSegmentFor(At);
    if (!Seg || At - Seg->VA >= Seg->Data.size() || !Dec.selectMode(Img, At))
      return std::nullopt;
    const size_t Offset = static_cast<size_t>(At - Seg->VA);
    DecodedExecutionUnit Unit;
    const int Size = Dec.decodeUnitForLift(Seg->Data.data() + Offset,
                                           Seg->Data.size() - Offset, At, Unit);
    if (Size <= 0 || Unit.HasDelaySlot)
      return std::nullopt;
    std::vector<LowOp> Ops;
    try {
      Dec.liftUnitToLow(Unit, Ops);
    } catch (const UnliftedInstruction &) {
      return std::nullopt;
    }
    if (llvm::any_of(Ops,
                     [](const LowOp &Op) { return Op.Opcode == NdOp::RETURN; }))
      return Body;
    if (llvm::any_of(Ops, [&](const LowOp &Op) {
          return leavesStraightLineBody(Op) ||
                 (Op.Output.isReg() && Op.Output.Offset == StackPointer);
        }))
      return std::nullopt;
    Body.insert(Body.end(), Ops.begin(), Ops.end());
    At += static_cast<va_t>(Size);
  }
  return std::nullopt;
}

std::optional<FixedIntegerCallArguments>
goInlinedRoutineOperands(const BinaryImage &Img, va_t Entry) {
  if (Img.Arch != Arch::AArch64)
    return std::nullopt;
  const std::optional<std::vector<LowOp>> Body =
      goInlinedRoutineBody(Img, Entry);
  if (!Body)
    return std::nullopt;
  const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
  // X0 to X30, but those Go code keeps a role for and the platform's.
  const auto Operand = [&](const NdVar &V) {
    return V.isReg() && V.Offset <= a64reg::X30 &&
           V.Offset % TRI.FullRegWidth == 0 &&
           !goStackABIEntryRegister(TRI, V.Offset) &&
           !TRI.isPlatformRegister(V.Offset);
  };
  // The registers the body reads before it writes them, in the order it
  // first reads them.  A write of a W register clears the upper half, so
  // any write defines the register.
  llvm::SmallVector<uint64_t, 32> Read, Written;
  for (const LowOp &Op : *Body) {
    for (unsigned I = 0; I < Op.NumInputs; ++I)
      if (const NdVar &V = Op.Inputs[I];
          Operand(V) && !llvm::is_contained(Written, V.Offset) &&
          !llvm::is_contained(Read, V.Offset))
        Read.push_back(V.Offset);
    if (Operand(Op.Output) && !llvm::is_contained(Written, Op.Output.Offset))
      Written.push_back(Op.Output.Offset);
  }
  const llvm::ArrayRef<uint64_t> Platform =
      TRI.integerArgumentLayout(Img.abiFormat()).Registers;
  const llvm::ArrayRef<uint64_t> Go = goIntegerArgumentRegisters(Img.Arch);
  if (llvm::all_of(Read, [&](uint64_t Register) {
        return llvm::is_contained(Platform, Register) ||
               llvm::is_contained(Go, Register);
      }))
    return std::nullopt;
  FixedIntegerCallArguments Arguments;
  const auto Add = [&](uint64_t Register) {
    if (llvm::is_contained(Read, Register) &&
        llvm::none_of(Arguments.Parameters,
                      [&](const FixedIntegerCallArguments::Parameter &P) {
                        return P.Register == Register;
                      }))
      Arguments.Parameters.push_back({SourceABICarrierKind::IntegerRegister,
                                      Register, 0,
                                      static_cast<uint16_t>(TRI.FullRegWidth)});
  };
  for (const uint64_t Register : Platform)
    Add(Register);
  for (const uint64_t Register : Go)
    Add(Register);
  llvm::sort(Read);
  for (const uint64_t Register : Read)
    Add(Register);
  return Arguments;
}

} // namespace neverd
