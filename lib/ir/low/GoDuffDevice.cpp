//===- GoDuffDevice.cpp - Calls into Go's Duff's device -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Lifts a Duff's device routine's straight-line body from an entry to its
/// return, which a call to that entry runs in its caller's place.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/GoDuffDevice.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/GoCallContract.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/STLExtras.h"

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

std::optional<std::vector<LowOp>> goDuffDeviceBody(const BinaryImage &Img,
                                                   va_t Entry) {
  if (!goDuffDeviceAt(Img, Entry))
    return std::nullopt;
  // A decoder of its own: the caller's still holds the call it decoded.
  Decoder Dec;
  if (!Dec.init(Img))
    return std::nullopt;
  const uint64_t StackPointer = getTargetRegInfo(Img.Arch).StackPointer;
  std::vector<LowOp> Body;
  // The routine's extent bounds the walk: each instruction lies inside it.
  for (va_t At = Entry; goDuffDeviceAt(Img, At);) {
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

} // namespace neverd
