//===- X86RegistrationFrame.cpp - x86 runtime root expressions ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/high/X86RegistrationFrame.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/X86RegistrationCallback.h"
#include "neverd/ir/med/X86RegistrationFrame.h"

namespace neverd {

ExprPtr x86RegistrationFrameAddress(const RegistrationFrameCoordinate &Frame,
                                    int32_t Offset) {
  auto Coordinate = Frame;
  int64_t FinalOffset = int64_t(Coordinate.AlignedOffset) + Offset;
  if (Coordinate.Alignment == 1) {
    FinalOffset += Coordinate.EntryOffset;
    Coordinate.EntryOffset = 0;
  }
  if (FinalOffset < INT32_MIN || FinalOffset > INT32_MAX)
    return nullptr;
  MedVar EntrySP;
  EntrySP.Kind = MedVar::Reg;
  EntrySP.TheArch = Arch::X86;
  EntrySP.RegOff = getTargetRegInfo(Arch::X86).StackPointer;
  EntrySP.Size = 4;
  auto Value = HighExpr::makeVar(EntrySP);
  auto AddOffset = [&](int32_t Offset) {
    if (Offset)
      Value = HighExpr::makeBinop(
          Offset < 0 ? NdOp::INT_SUB : NdOp::INT_ADD, Value,
          HighExpr::makeConst(Offset < 0 ? -int64_t(Offset) : int64_t(Offset),
                              4, ConstantAddressProvenance::Scalar));
  };
  AddOffset(Coordinate.EntryOffset);
  if (Coordinate.Alignment != 1)
    Value = HighExpr::makeBinop(
        NdOp::INT_AND, Value,
        HighExpr::makeConst(uint32_t(-Coordinate.Alignment), 4,
                            ConstantAddressProvenance::Scalar));
  AddOffset(int32_t(FinalOffset));
  return Value;
}

ExprPtr lowerX86RegistrationRoot(const MedFunc &Func, const MedOp &Op) {
  if (isRegistrationCallbackStackRoot(Func, Op)) {
    auto Value = HighExpr::makeVar(Op.Output, NdType::makeInt(4, false));
    Value->Kind = ExprKind::EntryRegister;
    Value->EntryFunctionVA = Func.Entry;
    Value->EntryVA = Op.Addr;
    return Value;
  }
  const auto Coordinate = registrationRootFrameCoordinate(Func, Op);
  if (!Coordinate)
    return HighExpr::makeUndef(Op.Output.Size);
  auto Address = x86RegistrationFrameAddress(*Coordinate);
  return Address ? Address : HighExpr::makeUndef(Op.Output.Size);
}

} // namespace neverd
