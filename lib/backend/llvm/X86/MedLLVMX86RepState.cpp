//===- MedLLVMX86RepState.cpp - Native REP completion --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Native intrinsics and the older-SDK assembly path share one full-register
/// result contract. Address-size arithmetic never substitutes for completion.
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/X86/X86RepStateAsm.h"
#include "neverd/ir/med/IntrinsicShapes.h"

#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/ErrorHandling.h"

#include <array>
#include <stdexcept>
#include <vector>

namespace neverd {

llvm::Value *MedLLVMEmitter::emitX86RepState(const MedOp &Op, Intrinsic Id,
                                             llvm::IRBuilder<> &B) {
  const auto Shape = x86RepMedShape(Op, TargetArch);
  if (!x86RepShapeIsValid(Id, Shape))
    llvm::report_fatal_error("invalid x86 REP completion contract");
  const auto P = *x86RepAsm(Id, TargetArch, *Shape.Descriptor);
  auto *Word = sizeToType(P.WordBytes);
  auto Coerce = [&](llvm::Value *V, llvm::Type *T) {
    return V->getType()->isPointerTy() ? B.CreatePtrToInt(V, T)
                                       : B.CreateZExtOrTrunc(V, T);
  };
  const bool Copy = Id == Intrinsic::X86RepMovsState;
  const auto &Count = Op.Inputs[Copy ? 3 : 2];
  const bool BoundedMemory =
      Img && (Copy || Id == Intrinsic::X86RepStosState) && !P.Control.Segment &&
      P.Control.AddressBytes == P.WordBytes && Count.isConst() &&
      Count.ConstVal && Op.Inputs[4].isConst();
  std::array<llvm::Value *, 2> OriginalAddresses{}, MemoryAddresses{};
  auto Input = [&](unsigned I) -> llvm::Value * {
    const auto &V = Op.Inputs[I];
    // Counts and unproven immediate fill patterns are integer values, even
    // when their bits coincide with an address in the image.
    if ((Copy || Id == Intrinsic::X86RepStosState) &&
        (I == (Copy ? 3u : 2u) || (!Copy && I == 3 && V.isConst() &&
                                   !isExactAddressProvenance(V.Provenance))))
      return getRawSegmentOffset(V, B);
    if (BoundedMemory && I <= (Copy ? 2u : 1u)) {
      const auto Address =
          V.isConst() ? std::optional<uint64_t>(V.ConstVal) : traceSSAConst(V);
      const auto *Segment = Address ? Img->getSegmentFor(*Address) : nullptr;
      if (Segment && Segment->isReadable() && !Segment->isExecutable()) {
        const bool Backward =
            (Op.Inputs[4].ConstVal >> X86RepDirectionShift) & 1;
        const auto Span =
            x86RepAccessSpan(*Address, Count.ConstVal, P.Control, Backward,
                             Segment->VA, Segment->Size);
        if (!Span)
          throw std::invalid_argument(
              "repeated image access crosses its mapped storage");
        // REP owns the memory role that a neutral LEA/COPY cannot decide.
        // Request every accessed byte, starting at the low end for DF=1.
        // Reuse ordinary data storage so adjacent relocated pointers do not
        // leave this operand at its original, unmapped numeric address.
        llvm::Constant *Base = nullptr;
        if (Span->Bytes <= UINT16_MAX)
          Base = tryResolveDirectDataBytes(Span->Begin,
                                           static_cast<uint16_t>(Span->Bytes));
        if (!Base)
          Base = tryResolveOwnedGlobalData(Span->Begin, Segment->VA, 0,
                                           /*WholeOwner=*/true);
        if (!Base)
          throw std::invalid_argument(
              "cannot materialize repeated image access storage");
        auto *Pointer =
            B.CreateConstGEP1_64(B.getInt8Ty(), Base, *Address - Span->Begin);
        OriginalAddresses[I - 1] = Coerce(getVar(V, B), Word);
        MemoryAddresses[I - 1] = Coerce(Pointer, Word);
        return Pointer;
      }
    }
    return getVar(V, B);
  };
  std::vector<llvm::Value *> Arguments;
  for (unsigned I = 1; I <= 3; ++I)
    Arguments.push_back(
        Coerce(Input(I), I == 3 && P.Contract.Port ? sizeToType(2) : Word));
  auto *Status = Coerce(getVar(Op.Inputs[4], B), sizeToType(X86RepStatusBytes));
  auto *Direction = B.CreateAnd(B.CreateLShr(Status, X86RepDirectionShift), 1);
  auto *Flags = B.CreateAnd(Status, X86RepFlagsMask);
  const auto NativeID =
      llvm::Intrinsic::lookupIntrinsicID(x86RepNativeName(Id, TargetArch));
  auto *Function = B.GetInsertBlock()->getParent();
  if (P.Contract.Compare && TargetArch == Arch::X64) {
    auto Features =
        Function->getFnAttribute("target-features").getValueAsString().str();
    Function->addFnAttr("target-features",
                        Features.empty() ? "+sahf" : Features + ",+sahf");
  }
  llvm::CallInst *Call;
  if (NativeID) {
    Arguments.push_back(Coerce(Direction, sizeToType(1)));
    if (P.Contract.Compare)
      Arguments.push_back(Coerce(Flags, sizeToType(X86RepFlagBytes)));
    Arguments.push_back(B.getInt32(P.Control.ElementBytes));
    Arguments.push_back(B.getInt32(P.Control.AddressBytes * 8));
    if (P.Contract.Source)
      Arguments.push_back(B.getInt32(P.Control.Segment));
    if (P.Contract.Compare)
      Arguments.push_back(B.getInt32(P.Control.NotEqual));
    Call = B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                            Function->getParent(), NativeID),
                        Arguments);
  } else {
    Arguments.push_back(Direction);
    if (P.Contract.Compare)
      Arguments.push_back(Coerce(Flags, Word));
    std::vector<llvm::Type *> Types;
    for (const auto *V : Arguments)
      Types.push_back(V->getType());
    auto *Result = llvm::StructType::get(
        *Ctx, std::vector<llvm::Type *>(P.OutputConstraints.size(), Word));
    auto *Signature = llvm::FunctionType::get(Result, Types, false);
    auto *Asm = llvm::InlineAsm::get(Signature, P.Text, P.Constraints, true);
    Call = B.CreateCall(Asm, Arguments);
  }
  auto Complete = [&](unsigned I) {
    llvm::Value *V = B.CreateExtractValue(Call, I);
    if (I < OriginalAddresses.size() && OriginalAddresses[I])
      // The memory role can use a different backing, but the architectural
      // result must retain the input's coordinate model. Use the hardware's
      // actual completion delta, never count-based reconstruction.
      V = B.CreateAdd(B.CreateSub(V, MemoryAddresses[I]), OriginalAddresses[I]);
    return V;
  };
  PendingIntrinsicCount = P.Contract.auxiliaryOutputs();
  for (unsigned I = 0; I < PendingIntrinsicCount; ++I) {
    auto *V = Complete(I + 1);
    if (I + 1 == P.Contract.WordResults)
      V = B.CreateAnd(Coerce(V, sizeToType(X86RepFlagBytes)), X86RepFlagsMask);
    PendingIntrinsicOutputs[I] = V;
  }
  return Complete(0);
}

} // namespace neverd
