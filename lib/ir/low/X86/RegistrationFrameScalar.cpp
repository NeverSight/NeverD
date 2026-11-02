//===- RegistrationFrameScalar.cpp - PE32 relative scalar results --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exact differences and equality are independent of a shared frame base.

#include "RegistrationFrame.h"

#include "neverd/ir/low/LowIR.h"

namespace neverd::registration_state {

std::optional<FrameValue>
FrameTransfer::evaluateRelativeScalar(const LowOp &Op) const {
  const bool Difference = Op.Opcode == NdOp::INT_SUB;
  const bool Ordered =
      Op.Opcode == NdOp::INT_LESS || Op.Opcode == NdOp::INT_LESSEQUAL ||
      Op.Opcode == NdOp::INT_SLESS || Op.Opcode == NdOp::INT_SLESSEQUAL;
  if ((!Difference && Op.Opcode != NdOp::INT_EQUAL &&
       Op.Opcode != NdOp::INT_NOTEQUAL && !Ordered) ||
      Op.NumInputs != 2 || Op.Inputs[0].Size != 4 || Op.Inputs[1].Size != 4 ||
      Op.Output.Size != (Difference ? 4 : 1))
    return std::nullopt;
  const auto Left = read(Op.Inputs[0]);
  const auto Right = read(Op.Inputs[1]);
  // Copies of one complete image identity remain equal after rebasing or
  // moving that object. This grants only its zero difference/self-relation,
  // never a numeric image address or an ordering of different addresses.
  if (Left.ImageAddress && Left == Right &&
      Left == FrameValue::imageAddress(*Left.ImageAddress))
    return FrameValue::constant(Op.Opcode == NdOp::INT_EQUAL ||
                                Op.Opcode == NdOp::INT_LESSEQUAL ||
                                Op.Opcode == NdOp::INT_SLESSEQUAL);
  if (Ordered)
    return std::nullopt;
  std::optional<std::pair<int32_t, int32_t>> Offsets;
  if (Left.Offset && Right.Offset && Left == FrameValue::frame(*Left.Offset) &&
      Right == FrameValue::frame(*Right.Offset))
    Offsets = {*Left.Offset, *Right.Offset};
  else if (Left.EntryOffset && Right.EntryOffset &&
           Left == FrameValue::entryFrame(*Left.EntryOffset) &&
           Right == FrameValue::entryFrame(*Right.EntryOffset))
    Offsets = {*Left.EntryOffset, *Right.EntryOffset};
  else if (Left.CallbackAddress && Right.CallbackAddress &&
           Left.CallbackAddress->Entry == Right.CallbackAddress->Entry &&
           Left == FrameValue::callbackFrame(Left.CallbackAddress->Entry,
                                             Left.CallbackAddress->Offset) &&
           Right == FrameValue::callbackFrame(Right.CallbackAddress->Entry,
                                              Right.CallbackAddress->Offset))
    Offsets = {Left.CallbackAddress->Offset, Right.CallbackAddress->Offset};
  if (!Offsets)
    return std::nullopt;
  // PE32 arithmetic wraps at 32 bits. A common base cancels in subtraction
  // and equality, but not in ordered comparisons across that wrap boundary.
  // Complete value identity above excludes catch objects, cookie expressions
  // and any mixed provenance even when their numeric offsets happen to agree.
  if (Difference)
    return FrameValue::constant(uint32_t(Offsets->first) -
                                uint32_t(Offsets->second));
  return FrameValue::constant(Op.Opcode == NdOp::INT_EQUAL
                                  ? Offsets->first == Offsets->second
                                  : Offsets->first != Offsets->second);
}

} // namespace neverd::registration_state
