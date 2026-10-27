//===- COFFRegistrationSEHBoundCall.cpp - Preserved SEH argument proof ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// Replay preserved x86 SEH helper argument contracts before installation.

#include "COFFRegistrationSEHJump.h"

#include "neverd/Limits.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/RegistrationImageProof.h"
#include "neverd/backend/llvm/X86RegistrationSEHBoundCall.h"
#include "neverd/backend/llvm/X86RegistrationSEHJump.h"

namespace neverd::coff_registration {

llvm::Expected<RegistrationSEHJumpContract> bindSEHBoundCalls(
    const MedFunc &Source, const llvm::Function &Parent,
    const RegistrationFrame &Frame,
    const std::map<va_t, const llvm::AllocaInst *> &Buffers,
    const std::map<CallbackKey, const llvm::Function *> &Callbacks) {
  const auto Expected = getX86RegistrationSEHBoundCalls(Source);
  if (!Expected)
    return rejectIR("preserved SEH helper has no complete argument contract");
  RegistrationSEHJumpContract Result;
  std::set<va_t> Seen;
  const auto &Layout = Parent.getParent()->getDataLayout();
  size_t Work = 0;
  std::vector<const llvm::Function *> Functions{&Parent};
  for (const auto &[Key, Callback] : Callbacks)
    Functions.push_back(Callback);
  for (const auto *Function : Functions)
    for (const auto &Block : *Function)
      for (const auto &I : Block) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return rejectIR(
              "preserved SEH argument proof exceeds its work budget");
        const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
        const auto *Marker =
            I.getMetadata(windows_eh_md::RegistrationOperationAttachment);
        const auto Address =
            Marker ? metadataInteger(*Marker, 1, 64) : std::nullopt;
        if (!Call || !Address || !Expected->count(*Address))
          continue;
        const auto &Bound = Expected->at(*Address);
        const auto &Receipt = *Bound.Source;
        const llvm::AllocaInst *Storage = Frame.Slot;
        int64_t Base = int64_t(Frame.EntrySP) - 4;
        if (Receipt.EntryVA) {
          const auto Owner = Callbacks.find({Receipt.EntryVA, true});
          if (Owner == Callbacks.end() || Owner->second != Function)
            return rejectIR("SEH runtime call belongs to another invocation");
          const auto Stack = getX86RegistrationSEHJumpStack(*Function);
          if (!Stack)
            return rejectIR("SEH runtime call lost its private callback stack");
          Storage = Stack->Slot;
          Base = Stack->EntrySP;
        } else if (Function != &Parent)
          return rejectIR("SEH parent runtime call moved into a callback");
        const auto Bytes = Storage->getAllocationSize(Layout);
        const auto *Callee = Call->getCalledFunction();
        const auto Convention = getX86RegistrationSEHBoundConvention(Receipt);
        const auto Import = getX86RegistrationSEHBoundImport(Receipt);
        if (!matchesSourceOperation(I, Source.Entry, *Address, 2) ||
            metadataInteger(*Marker, 2, 32) != uint32_t(Receipt.OpSeq) ||
            metadataInteger(*Marker, 3, 32) != uint32_t(Bound.Block) ||
            !Seen.insert(*Address).second || !Callee ||
            !Callee->isDeclaration() || !Callee->hasExternalLinkage() ||
            Callee->getCallingConv() != Convention ||
            Call->getCallingConv() != Convention ||
            Callee->hasDLLImportStorageClass() != !Import.empty() ||
            (!Import.empty() && Callee->getName() != Import) ||
            Callee->getFunctionType() !=
                getX86RegistrationSEHBoundType(Receipt, Parent.getContext()) ||
            Call->arg_size() != Receipt.Arguments.size() ||
            Call->getNumOperandBundles() || Call->isTailCall() ||
            Call->getMemoryEffects() != llvm::MemoryEffects::unknown() ||
            Call->doesNotThrow() ||
            Call->hasFnAttr(llvm::Attribute::NoCallback) ||
            Call->hasFnAttr(llvm::Attribute::WillReturn) ||
            Call->hasFnAttr(llvm::Attribute::ReturnsTwice) ||
            (!Call->use_empty() && !Bound.ObservesScalarReturn) ||
            Call->doesNotReturn() == Receipt.HasOrdinaryReturn)
          return rejectIR("preserved SEH helper changed its invocation ABI");
        auto Identity = rewrite_source::getOriginalVA(*Callee);
        if (!Identity)
          return Identity.takeError();
        if (!*Identity || **Identity != Receipt.TargetVA)
          return rejectIR("preserved SEH helper changed its original target");
        if (Callee->getAttributes().getRetAttrs().hasAttributes() ||
            Callee->getAttributes().getFnAttrs().hasAttributes() ||
            Call->getAttributes().getRetAttrs().hasAttributes())
          return rejectIR("preserved SEH helper gained unproved attributes");
        for (const auto &Attr : Call->getAttributes().getFnAttrs())
          if (!Attr.isEnumAttribute() ||
              Attr.getKindAsEnum() != llvm::Attribute::NoReturn)
            return rejectIR("preserved SEH helper gained unproved effects");
        std::set<const llvm::Instruction *> Arguments;
        const llvm::LoadInst *FirstScalar = nullptr;
        for (size_t Index = 0; Index != Receipt.Arguments.size(); ++Index) {
          if (++Work > limits::kMaxRegistrationEHStateWork)
            return rejectIR("preserved SEH arguments exceed the work budget");
          const auto &Arg = Receipt.Arguments[Index];
          const auto *Value = Call->getArgOperand(Index);
          if (Call->getAttributes().getParamAttrs(Index).hasAttributes() ||
              Callee->getAttributes().getParamAttrs(Index).hasAttributes())
            return rejectIR("preserved SEH helper changed its parameter ABI");
          using Kind = RegistrationSEHBoundArgument::Kind;
          if (Arg.ArgumentKind == Kind::NullPointer) {
            if (!llvm::isa<llvm::ConstantPointerNull>(Value))
              return rejectIR("closed SEH runtime gained an argument array");
            continue;
          }
          if (Arg.ArgumentKind == Kind::Unused) {
            if (integer(Value, 32) != 0)
              return rejectIR("unused SEH helper argument gained a value");
            continue;
          }
          if (Arg.ArgumentKind == Kind::ConstantScalar) {
            if (integer(Value, 32) != Arg.ScalarValue)
              return rejectIR("SEH runtime changed its exact scalar argument");
            continue;
          }
          if (Arg.ArgumentKind == Kind::ImagePointer) {
            if (registration_frame::originalImagePointerArgumentAddress(
                    *Call, Index, Work) != Arg.ImageVA)
              return rejectIR("SEH runtime changed its original image pointer");
            continue;
          }
          if (Arg.ArgumentKind == Kind::JumpBuffer) {
            const auto Buffer = Buffers.find(Arg.Capture);
            if (Buffer == Buffers.end() || !Buffer->second ||
                Value != Buffer->second)
              return rejectIR("preserved SEH helper borrowed another capture");
            continue;
          }
          if (Arg.ArgumentKind == Kind::Scalar) {
            const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Value);
            if (!Load || !Load->hasOneUse() || !Load->isVolatile() ||
                Load->isAtomic() || Load->getAlign() != llvm::Align(1) ||
                !Load->getType()->isIntegerTy(32) ||
                Load->getParent() != Call->getParent() ||
                !Load->comesBefore(Call))
              return rejectIR("preserved SEH helper lost its scalar argument");
            Arguments.insert(Load);
            if (!FirstScalar || Load->comesBefore(FirstScalar))
              FirstScalar = Load;
            Value = Load->getPointerOperand();
          }
          const auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(Value);
          llvm::APInt Offset(32, 0);
          const int64_t ExpectedOffset = Base + Arg.Offset;
          const auto Width = Arg.ArgumentKind == Kind::Object
                                 ? Receipt.borrowedObjectBytes()
                                 : 4;
          if (!GEP || GEP->getPointerOperand() != Storage ||
              !GEP->accumulateConstantOffset(Layout, Offset) ||
              ExpectedOffset < 0 || !Bytes || Bytes->isScalable() ||
              uint64_t(ExpectedOffset) + Width > Bytes->getFixedValue() ||
              Offset.getZExtValue() != uint64_t(ExpectedOffset))
            return rejectIR("preserved SEH helper changed its parent argument");
          Arguments.insert(GEP);
        }
        // Scalar slots must be sampled at the original invocation, after all
        // source stores. Only the checked argument projection and an invoke's
        // independently authenticated provenance anchor may follow those reads.
        for (const llvm::Instruction *I = FirstScalar; I && I != Call;
             I = next(*I)) {
          if (++Work > limits::kMaxRegistrationEHStateWork)
            return rejectIR(
                "preserved SEH argument reads exceed the work budget");
          if (Arguments.count(I))
            continue;
          const auto *Anchor = llvm::dyn_cast<llvm::IntrinsicInst>(I);
          if (!llvm::isa<llvm::InvokeInst>(Call) || !Anchor ||
              Anchor->getIntrinsicID() != llvm::Intrinsic::sideeffect ||
              Anchor->arg_size() || Anchor->getNumOperandBundles() != 1 ||
              Anchor->countOperandBundlesOfType(
                  windows_eh_md::ProvenanceBundle) != 1 ||
              next(*Anchor) != Call)
            return rejectIR(
                "preserved SEH scalar moved across source execution");
        }
        Result.Calls.insert(Call);
        if (Receipt.CalleeKind ==
            RegistrationSEHBoundCall::Kind::GetStartupInfoW)
          Result.RuntimeOutputs.emplace(
              Call, RegistrationSEHJumpContract::Output{
                        Storage, Base + Receipt.Arguments.front().Offset,
                        Receipt.borrowedObjectBytes()});
        if (Receipt.CalleeKind ==
                RegistrationSEHBoundCall::Kind::HeapTermination ||
            Receipt.CalleeKind ==
                RegistrationSEHBoundCall::Kind::InterlockedCompareExchange ||
            Receipt.CalleeKind == RegistrationSEHBoundCall::Kind::Sleep)
          Result.NoFrameBorrowCalls.insert(Call);
      }
  if (Result.Calls.size() != Expected->size())
    return rejectIR("preserved SEH helper call set is incomplete");
  return Result;
}

} // namespace neverd::coff_registration
