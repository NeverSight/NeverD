//===- MedLLVMMIPSScalar.cpp - MIPS scalar LLVM lowering -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve byte-level merge-load footprints and reject observable undefined
/// architectural results instead of substituting LLVM poison or zero.
//===----------------------------------------------------------------------===//

#include "neverd/ArchSupport.h"
#include "neverd/backend/MIPS/MIPSDSPSource.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/med/MIPS/MIPSIntrinsicShapes.h"

#include "llvm/IR/Intrinsics.h"

#include <stdexcept>

namespace neverd {

#include "neverd/backend/MIPS/MIPSMemorySource.inc"

llvm::Value *MedLLVMEmitter::emitMIPSScalar(const MedOp &Op, Intrinsic Id,
                                            llvm::IRBuilder<> &B) {
  if (!mipsScalarMedShapeIsValid(Op, TargetArch))
    throw std::runtime_error(mips::kMalformedScalar);
  if (Id == Intrinsic::MIPSUnpredictable)
    throw std::runtime_error(mips::kUnpredictableResult);
  if (Id == Intrinsic::MIPSLwrUpper)
    throw std::runtime_error(mips::kPartialLwrRequiresWord);
  if (Id == Intrinsic::MIPSReadDSPControl ||
      Id == Intrinsic::MIPSWriteDSPControl) {
    mips::applyDSPSourceAttributes(*B.GetInsertBlock()->getParent());
    const bool Write = Id == Intrinsic::MIPSWriteDSPControl;
    llvm::SmallVector<llvm::Type *, 1> Parameters;
    llvm::SmallVector<llvm::Value *, 1> Arguments;
    if (Write) {
      Parameters.push_back(B.getInt32Ty());
      auto *Value = getVar(Op.Inputs[1], B);
      Arguments.push_back(Value->getType()->isPointerTy()
                              ? B.CreatePtrToInt(Value, B.getInt32Ty())
                              : B.CreateZExtOrTrunc(Value, B.getInt32Ty()));
    }
    auto *Type = llvm::FunctionType::get(Write ? B.getVoidTy() : B.getInt32Ty(),
                                         Parameters, false);
    return B.CreateCall(
        llvm::InlineAsm::get(
            Type, Write ? mips::DSPWriteAsm : mips::DSPReadAsm,
            Write ? mips::DSPWriteConstraints : mips::DSPReadConstraints, true),
        Arguments);
  }
  const auto Coerce = [&](llvm::Value *V, llvm::Type *T) {
    return V->getType()->isPointerTy() ? B.CreatePtrToInt(V, T)
                                       : B.CreateZExtOrTrunc(V, T);
  };
  const auto Require = [&](llvm::Value *Valid, const char *Domain) {
    auto *Function = B.GetInsertBlock()->getParent();
    auto *Trap =
        llvm::BasicBlock::Create(*Ctx, llvm::Twine(Domain) + ".trap", Function);
    auto *Continue =
        llvm::BasicBlock::Create(*Ctx, llvm::Twine(Domain) + ".ok", Function);
    B.CreateCondBr(Valid, Continue, Trap);
    B.SetInsertPoint(Trap);
    B.CreateCall(
        llvm::Intrinsic::getOrInsertDeclaration(Mod, llvm::Intrinsic::trap));
    B.CreateUnreachable();
    B.SetInsertPoint(Continue);
  };
  if (Id == Intrinsic::MIPSRequireWord) {
    auto *First =
        Coerce(getVar(Op.Inputs[1], B), sizeToType(Op.Inputs[1].Size));
    auto *Word =
        B.CreateSExt(B.CreateTrunc(First, B.getInt32Ty()), First->getType());
    Require(B.CreateICmpEQ(First, Word), "mips.word");
    return nullptr;
  }
  if (mips::isWordDivision(Id)) {
    auto *A = Coerce(getVar(Op.Inputs[1], B), B.getInt32Ty());
    auto *D = Coerce(getVar(Op.Inputs[2], B), B.getInt32Ty());
    // This qualifies the source domain, not an architectural divide exception.
    Require(B.CreateICmpNE(D, B.getInt32(0)), "mips.divisor.domain");
    llvm::Value *X = A, *Y = D;
    auto *NegativeA = B.CreateICmpSLT(A, B.getInt32(0));
    auto *NegativeD = B.CreateICmpSLT(D, B.getInt32(0));
    if (Id == Intrinsic::MIPSDivWord) {
      X = B.CreateSelect(NegativeA, B.CreateNeg(A), A);
      Y = B.CreateSelect(NegativeD, B.CreateNeg(D), D);
    }
    llvm::Value *Q = B.CreateUDiv(X, Y), *R = B.CreateURem(X, Y);
    if (Id == Intrinsic::MIPSDivWord) {
      Q = B.CreateSelect(B.CreateXor(NegativeA, NegativeD), B.CreateNeg(Q), Q);
      R = B.CreateSelect(NegativeA, B.CreateNeg(R), R);
    }
    return B.CreateOr(B.CreateShl(B.CreateZExt(R, B.getInt64Ty()), 32),
                      B.CreateZExt(Q, B.getInt64Ty()));
  }

  const auto Load = *mips::mergeMemory(Id);
  const bool Store = mips::mergeStore(Id).has_value();
  const bool Little = archLittleEndian(TargetArch);
  const auto &AddressValue = Op.Inputs[1];
  auto VA = AddressValue.isConst()
                ? std::optional<uint64_t>(AddressValue.ConstVal)
                : traceSSAConst(AddressValue);
  if (VA && AddressValue.Size == 4)
    VA = uint32_t(*VA);
  // Keep guest alignment bits separate from a relocated byte-array pointer.
  auto *First =
      VA ? llvm::ConstantInt::get(sizeToType(AddressValue.Size), *VA)
         : Coerce(getVar(AddressValue, B), sizeToType(AddressValue.Size));
  auto *AddressTy = First->getType();
  auto *ValueTy = sizeToType(Load.Bytes);
  auto *OutputTy = Store ? ValueTy : sizeToType(Op.Output.Size);
  auto *Old = Coerce(getVar(Op.Inputs[2], B), OutputTy);
  const auto C = [&](uint64_t V) {
    return llvm::ConstantInt::get(AddressTy, V);
  };
  auto *Offset = B.CreateAnd(First, C(Load.Bytes - 1));
  const bool Forward = Load.Left != Little;
  auto *Count =
      Forward ? B.CreateSub(C(Load.Bytes), Offset) : B.CreateAdd(Offset, C(1));
  auto *Base = Forward ? First : B.CreateSub(First, Offset);
  auto *DestinationByte = Load.Left ? B.CreateSub(C(Load.Bytes), Count) : C(0);
  llvm::Value *Memory = nullptr;
  if (VA) {
    const auto Access = mips::mergeAccess(Load, *VA, Little);
    if (Store && Img) {
      if (const auto *Segment = Img->getSegmentFor(Access.First);
          Segment && !Segment->isWritable())
        throw std::runtime_error(kMIPSMergeImageAddress);
    }
    auto Begin = MedVar::makeConst(Access.First, AddressValue.Size);
    Begin.Provenance = AddressValue.Provenance;
    Begin.AddressOwnerVA = AddressValue.AddressOwnerVA;
    if (Img)
      Memory = tryResolveDirectGlobalDataAddress(Begin, Access.Count);
    if (!Memory && Img && Img->getSegmentFor(Access.First))
      throw std::runtime_error(kMIPSMergeImageAddress);
    if (!Memory)
      Memory = getMemoryPtr(Base, B.getInt8Ty(), B);
  } else {
    if (Img && !usesOriginalImageStorage() &&
        (tryResolveWritableData(AddressValue, 1, B) ||
         tryResolveReadOnlyDataPtr(AddressValue, 1, true, B)))
      throw std::runtime_error(kMIPSMergeImageAddress);
    rejectEscapingAddressFragment(AddressValue, kMIPSMergeImageAddress);
    Memory = getMemoryPtr(First, B.getInt8Ty(), B);
    if (!Forward)
      Memory = B.CreateGEP(B.getInt8Ty(), Memory, B.CreateNeg(Offset));
  }

  auto *Initial = B.CreateZExtOrTrunc(Old, ValueTy);
  auto *Entry = B.GetInsertBlock();
  auto *Function = Entry->getParent();
  auto *Loop = llvm::BasicBlock::Create(*Ctx, "mips.merge.byte", Function);
  auto *Done = llvm::BasicBlock::Create(*Ctx, "mips.merge.done", Function);
  B.CreateBr(Loop);
  B.SetInsertPoint(Loop);
  auto *Index = B.CreatePHI(AddressTy, 2, "mips.merge.index");
  auto *Result = Store ? nullptr : B.CreatePHI(ValueTy, 2, "mips.merge.value");
  Index->addIncoming(C(0), Entry);
  if (Result)
    Result->addIncoming(Initial, Entry);
  auto *Pointer = B.CreateGEP(B.getInt8Ty(), Memory, Index);
  auto *Lane = Little ? static_cast<llvm::Value *>(Index)
                      : B.CreateSub(B.CreateSub(Count, C(1)), Index);
  auto *Shift = B.CreateZExtOrTrunc(
      B.CreateShl(B.CreateAdd(DestinationByte, Lane), C(3)), ValueTy);
  llvm::Value *Next = nullptr;
  if (Store) {
    auto *Byte = B.CreateTrunc(B.CreateLShr(Initial, Shift), B.getInt8Ty());
    auto *Write = B.CreateStore(Byte, Pointer);
    Write->setVolatile(true);
    Write->setAlignment(llvm::Align(1));
  } else {
    auto *Byte = B.CreateLoad(B.getInt8Ty(), Pointer);
    // A discarded destination must still perform its original reads/faults.
    Byte->setVolatile(true);
    Byte->setAlignment(llvm::Align(1));
    auto *Mask = B.CreateShl(llvm::ConstantInt::get(ValueTy, 255), Shift);
    Next = B.CreateOr(B.CreateAnd(Result, B.CreateNot(Mask)),
                      B.CreateShl(B.CreateZExt(Byte, ValueTy), Shift));
  }
  auto *NextIndex = B.CreateAdd(Index, C(1));
  auto *Backedge = B.GetInsertBlock();
  B.CreateCondBr(B.CreateICmpULT(NextIndex, Count), Loop, Done);
  Index->addIncoming(NextIndex, Backedge);
  if (Result)
    Result->addIncoming(Next, Backedge);
  B.SetInsertPoint(Done);
  return Store ? nullptr : B.CreateSExtOrTrunc(Next, OutputTy);
}

} // namespace neverd
