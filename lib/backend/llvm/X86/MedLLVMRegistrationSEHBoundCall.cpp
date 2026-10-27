//===- MedLLVMRegistrationSEHBoundCall.cpp - Preserved helper arguments ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// Project preserved x86 SEH helper arguments onto the generated frame.

#include "MedLLVMRegistrationSEHJump.h"

#include "neverd/backend/RewriteSourceIdentity.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

namespace neverd::x86_registration {

bool prepareSEHBoundCall(const llvm::CallInst &Call,
                         const RegistrationSEHBoundCall &Source,
                         const llvm::AllocaInst &Frame, uint64_t EntrySP) {
  const auto *Callee = Call.getCalledFunction();
  if (!Callee || !Callee->isDeclaration() ||
      Callee->getFunctionType() !=
          getX86RegistrationSEHBoundType(Source, Call.getContext()) ||
      Call.getCallingConv() != getX86RegistrationSEHBoundConvention(Source) ||
      Call.arg_size() != Source.Arguments.size() || Call.isTailCall() ||
      Call.getNumOperandBundles())
    return false;
  auto Identity = rewrite_source::getOriginalVA(*Callee);
  if (!Identity) {
    llvm::consumeError(Identity.takeError());
    return false;
  }
  if (!*Identity || **Identity != Source.TargetVA)
    return false;
  return hasSEHBoundFrameArguments(Source, Frame, EntrySP);
}

bool hasSEHBoundFrameArguments(const RegistrationSEHBoundCall &Source,
                               const llvm::AllocaInst &Frame,
                               uint64_t EntrySP) {
  if (Source.EntryVA) {
    // The callback plan separately authenticates this invocation's ESP seed
    // and redirects every private access to its own bounded allocation.
    if (Source.CalleeKind != RegistrationSEHBoundCall::Kind::RaiseException ||
        Source.StackOffset > -16)
      return false;
    for (const auto &Arg : Source.Arguments)
      if (Arg.ArgumentKind == RegistrationSEHBoundArgument::Kind::Scalar &&
          (Arg.Offset < Source.StackOffset || Arg.Offset > -4))
        return false;
    return true;
  }
  const auto Bytes =
      Frame.getAllocationSize(Frame.getModule()->getDataLayout());
  if (!Bytes || Bytes->isScalable())
    return false;
  for (const auto &Arg : Source.Arguments) {
    using Kind = RegistrationSEHBoundArgument::Kind;
    if (Arg.ArgumentKind != Kind::Scalar && Arg.ArgumentKind != Kind::Object)
      continue;
    const int64_t Offset = int64_t(EntrySP) - 4 + Arg.Offset;
    const auto Width =
        Arg.ArgumentKind == Kind::Object ? Source.borrowedObjectBytes() : 4;
    if (Offset < 0 || uint64_t(Offset) + Width > Bytes->getFixedValue())
      return false;
  }
  return true;
}

llvm::SmallVector<llvm::Value *, 8>
projectSEHBoundArguments(const RegistrationSEHBoundCall &Source,
                         llvm::IRBuilder<> &B, llvm::AllocaInst &Frame,
                         uint64_t EntrySP,
                         const std::map<va_t, llvm::AllocaInst *> &Buffers,
                         const std::map<va_t, llvm::Value *> &ImagePointers) {
  llvm::SmallVector<llvm::Value *, 8> Arguments;
  for (const auto &Arg : Source.Arguments) {
    using Kind = RegistrationSEHBoundArgument::Kind;
    switch (Arg.ArgumentKind) {
    case Kind::Unused:
      Arguments.push_back(B.getInt32(0));
      break;
    case Kind::ConstantScalar:
      Arguments.push_back(B.getInt32(Arg.ScalarValue));
      break;
    case Kind::ImagePointer:
      Arguments.push_back(ImagePointers.at(Arg.ImageVA));
      break;
    case Kind::NullPointer:
      Arguments.push_back(llvm::ConstantPointerNull::get(B.getPtrTy()));
      break;
    case Kind::JumpBuffer:
      Arguments.push_back(Buffers.at(Arg.Capture));
      break;
    case Kind::Object:
    case Kind::Scalar: {
      auto *Slot = B.CreateInBoundsGEP(
          B.getInt8Ty(), &Frame, B.getInt32(int64_t(EntrySP) - 4 + Arg.Offset));
      if (Arg.ArgumentKind == Kind::Object)
        Arguments.push_back(Slot);
      else {
        auto *Value = B.CreateLoad(B.getInt32Ty(), Slot);
        Value->setAlignment(llvm::Align(1));
        Value->setVolatile(true);
        Arguments.push_back(Value);
      }
      break;
    }
    }
  }
  return Arguments;
}

std::map<llvm::CallInst *, llvm::CallInst *>
emitSEHBoundCalls(const SEHJumpPlan &Plan, llvm::AllocaInst &Frame,
                  uint64_t EntrySP,
                  const std::map<va_t, llvm::AllocaInst *> &Buffers) {
  std::map<llvm::CallInst *, llvm::CallInst *> Result;
  for (const auto &[Call, Source] : Plan.BoundCalls) {
    // Imported runtimes already received their exact source arguments during
    // call emission, including any observed scalar result.
    if (Source.Source->CalleeKind !=
        RegistrationSEHBoundCall::Kind::PreservedHelper)
      continue;
    llvm::IRBuilder<> B(Call);
    const auto Arguments =
        projectSEHBoundArguments(*Source.Source, B, Frame, EntrySP, Buffers);
    auto *Replacement = B.CreateCall(Call->getCalledFunction(), Arguments);
    Replacement->setCallingConv(
        getX86RegistrationSEHBoundConvention(*Source.Source));
    if (!Source.Source->HasOrdinaryReturn)
      Replacement->addFnAttr(llvm::Attribute::NoReturn);
    Replacement->copyMetadata(*Call);
    Result.emplace(Call, Replacement);
  }
  return Result;
}

} // namespace neverd::x86_registration
