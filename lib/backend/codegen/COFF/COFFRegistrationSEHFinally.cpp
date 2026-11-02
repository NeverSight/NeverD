//===- COFFRegistrationSEHFinally.cpp - Ordinary SEH invocation proof -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind parent and nested finally calls to the same recovered physical frame.

#include "COFFRegistrationSEHFinally.h"

#include "neverd/Limits.h"
#include "neverd/backend/llvm/X86/X86RegistrationSEHJump.h"
#include "neverd/ir/med/X86/X86RegistrationSEHCall.h"

namespace neverd::coff_registration {

llvm::Expected<std::set<const llvm::CallBase *>> bindSEHFinallyCalls(
    const MedFunc &Source, const llvm::Function &Parent,
    const std::map<CallbackKey, const llvm::Function *> &Callbacks,
    const std::map<int, SourceSegment> &Segments,
    llvm::ArrayRef<const llvm::Function *> Functions) {
  if (!Source.RegistrationStates ||
      !Source.RegistrationStates->SEHInvocationExitsComplete)
    return rejectIR("ordinary finally calls have incomplete source exits");
  size_t Work = 0;
  auto Charge = [&] { return ++Work <= limits::kMaxRegistrationEHStateWork; };
  struct SourceCall {
    va_t Target;
    int Block;
    int Sequence;
    const llvm::Function *Owner;
  };
  std::map<va_t, SourceCall> Expected;
  for (const auto &Block : Source.Blocks)
    for (const auto &Op : Block.Ops) {
      if (!Charge())
        return rejectIR("ordinary finally source exceeds its work budget");
      if (Op.Opcode != NdOp::CALL || Op.NumInputs != 1 ||
          !Op.Inputs[0].isConst() ||
          !Callbacks.count({Op.Inputs[0].ConstVal, true}))
        continue;
      const auto Segment = Segments.find(Block.Id);
      if (Segment == Segments.end() || !Segment->second.Enter ||
          !Expected
               .emplace(Op.Addr,
                        SourceCall{Op.Inputs[0].ConstVal, Block.Id,
                                   Op.OriginSeq,
                                   Segment->second.Enter->getFunction()})
               .second)
        return rejectIR("ordinary finally source invocation is ambiguous");
    }
  const auto Returns = getRegistrationSEHReturns(Source);
  const auto Nonlocal = getRegistrationSEHNonlocalCalls(Source);
  if (!Returns || !Nonlocal)
    return rejectIR("ordinary finally calls lost their source exits");
  std::set<const llvm::Function *> FinallyOwners, ReturningCallbacks;
  for (const auto &[Key, Callback] : Callbacks) {
    if (!Key.second)
      continue;
    FinallyOwners.insert(Callback);
    for (const auto &Block : *Callback) {
      if (!Charge())
        return rejectIR("ordinary finally callbacks exceed their work budget");
      if (llvm::isa<llvm::ReturnInst>(Block.getTerminator()))
        ReturningCallbacks.insert(Callback);
    }
  }
  std::set<const llvm::CallBase *> Result;
  std::set<va_t> Seen;
  for (const auto *Function : Functions)
    for (const auto &Block : *Function)
      for (const auto &I : Block) {
        if (!Charge())
          return rejectIR("ordinary finally proof exceeds its work budget");
        const auto *Marker =
            I.getMetadata(windows_eh_md::RegistrationFinallyCallAttachment);
        if (!Marker)
          continue;
        const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
        const auto Address = metadataInteger(*Marker, 1, 64);
        const auto Target = metadataInteger(*Marker, 2, 64);
        if (!Call || Marker->getNumOperands() != 3 ||
            metadataInteger(*Marker, 0, 64) != Source.Entry || !Address ||
            !Target || !Expected.count(*Address) ||
            !Seen.insert(*Address).second)
          return rejectIR("ordinary finally call changed its source identity");
        const auto &Origin = Expected.at(*Address);
        const auto *Operation =
            I.getMetadata(windows_eh_md::RegistrationOperationAttachment);
        const auto Callee = Callbacks.find({*Target, true});
        if (Origin.Target != *Target || Origin.Owner != Function ||
            !matchesSourceOperation(I, Source.Entry, *Address, 2) ||
            metadataInteger(*Operation, 2, 32) != uint32_t(Origin.Sequence) ||
            metadataInteger(*Operation, 3, 32) != uint32_t(Origin.Block) ||
            Callee == Callbacks.end() ||
            Call->getCalledFunction() != Callee->second ||
            Call->getFunctionType() != Callee->second->getFunctionType() ||
            Call->getCallingConv() != llvm::CallingConv::C ||
            Callee->second->getCallingConv() != llvm::CallingConv::C ||
            Call->arg_size() != 2 || integer(Call->getArgOperand(0), 8) != 0 ||
            !Call->use_empty() || Call->getNumOperandBundles() ||
            Call->isTailCall() ||
            (Call->doesNotReturn() && ReturningCallbacks.count(Callee->second)))
          return rejectIR(
              "ordinary finally call changed its normal parent-frame ABI");
        if (Call->getAttributes().getRetAttrs().hasAttributes() ||
            Call->getAttributes().getParamAttrs(0).hasAttributes() ||
            Call->getAttributes().getParamAttrs(1).hasAttributes())
          return rejectIR("ordinary finally call gained unproved attributes");
        for (const auto &Attribute : Call->getAttributes().getFnAttrs())
          if (!Attribute.isEnumAttribute() ||
              Attribute.getKindAsEnum() != llvm::Attribute::NoReturn)
            return rejectIR("ordinary finally call gained unproved effects");
        if (Function == &Parent) {
          const auto *Frame =
              llvm::dyn_cast<llvm::IntrinsicInst>(Call->getArgOperand(1));
          if (!Frame ||
              Frame->getIntrinsicID() != llvm::Intrinsic::localaddress ||
              Frame->arg_size() || Frame->getFunction() != &Parent)
            return rejectIR(
                "ordinary finally call changed its normal parent-frame ABI");
        } else {
          const auto *Frame =
              getX86RegistrationSEHJumpParentFrame(*Function, Parent);
          const auto Key =
              std::make_tuple(Origin.Block, *Address, Origin.Sequence);
          const auto Returned = Returns->ReturningCalls.find(Key);
          const auto Jumped = Nonlocal->find(Key);
          const bool NoReturn = !ReturningCallbacks.count(Callee->second);
          if (!FinallyOwners.count(Function) ||
              !llvm::isa<llvm::CallInst>(Call) || !Frame ||
              Call->getArgOperand(1) != Frame ||
              Call->doesNotReturn() != NoReturn ||
              (NoReturn
                   ? Returned != Returns->ReturningCalls.end() ||
                         Jumped == Nonlocal->end() || Jumped->second != *Target
                   : Returned == Returns->ReturningCalls.end() ||
                         Returned->second != *Target))
            return rejectIR(
                "nested finally call lost its frame or checked exit");
        }
        Result.insert(Call);
      }
  if (Seen.size() != Expected.size())
    return rejectIR("source ordinary finally call has no recovered callback");
  return Result;
}

} // namespace neverd::coff_registration
