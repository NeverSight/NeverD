//===- MedLLVMRegistrationSEHState.cpp - Callback state writes ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Translate checked source state stores to physical parent registration
/// writes.

#include "MedLLVMRegistrationSEHState.h"

#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/WindowsEHSemanticDigest.h"
#include "neverd/backend/llvm/X86/X86RegistrationSEHJump.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/X86/X86RegistrationSEHStateStore.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicsX86.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/BinaryRewrite.h"

#include <set>

namespace neverd::x86_registration {
namespace {

bool canDeclareIntrinsic(const llvm::Module &Module, llvm::Intrinsic::ID ID,
                         llvm::StringRef Name) {
  const auto *Value = Module.getNamedValue(Name);
  if (!Value)
    return true;
  const auto *Function = llvm::dyn_cast<llvm::Function>(Value);
  auto *Type = llvm::Intrinsic::getType(Module.getContext(), ID);
  return Function && Function->isDeclaration() &&
         Function->hasExternalLinkage() && !Function->getAddressSpace() &&
         Function->getCallingConv() == llvm::CallingConv::C &&
         Function->getIntrinsicID() == ID &&
         Function->getFunctionType() == Type &&
         Function->getAttributes() ==
             llvm::Intrinsic::getAttributes(Module.getContext(), ID, Type);
}

} // namespace

bool canPreserveSEHCallbackABI(const llvm::Function &Parent) {
  const auto *Value = Parent.getParent()->getNamedValue("llvm.compiler.used");
  if (!Value)
    return true;
  const auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(Value);
  const auto *Array =
      Global && Global->hasInitializer()
          ? llvm::dyn_cast<llvm::ConstantArray>(Global->getInitializer())
          : nullptr;
  return Global && Global->hasAppendingLinkage() &&
         Global->getSection() == "llvm.metadata" && !Global->isThreadLocal() &&
         !Global->getAddressSpace() && Array &&
         Array->getType()->getElementType() ==
             llvm::PointerType::getUnqual(Parent.getContext()) &&
         llvm::all_of(Array->operands(), [](const llvm::Use &Element) {
           return llvm::isa<llvm::GlobalValue>(Element->stripPointerCasts());
         });
}

bool canEmitSEHRetainedScopes(const llvm::Function &Parent) {
  return canDeclareIntrinsic(*Parent.getParent(), llvm::Intrinsic::donothing,
                             "llvm.donothing");
}

std::optional<std::vector<SEHCallbackStateWrite>> prepareSEHCallbackStateWrites(
    const MedFunc &Source, llvm::Function &Parent,
    llvm::ArrayRef<X86RegistrationCallbackRequest> Requests,
    const std::map<std::pair<va_t, int>, llvm::Instruction *> &Memory) {
  const auto Stores = getX86RegistrationSEHStateStores(Source);
  if (!Stores)
    return std::nullopt;
  std::vector<SEHCallbackStateWrite> Result;
#ifndef LLVM_NEVERD_X86_REGISTRATION_SEH_STATE_WRITES
  if (!Stores->empty())
    return std::nullopt;
#else
  if (!Stores->empty() &&
      !canDeclareIntrinsic(*Parent.getParent(),
                           llvm::Intrinsic::x86_seh_setstate,
                           "llvm.x86.seh.setstate"))
    return std::nullopt;
  std::set<const llvm::Instruction *> Seen;
  for (const auto &Bound : *Stores) {
    const auto &Receipt = *Bound.Source;
    const auto Found = Memory.find({Receipt.Address, Receipt.OpSeq});
    const auto *Store = Found == Memory.end()
                            ? nullptr
                            : llvm::dyn_cast<llvm::StoreInst>(Found->second);
    const auto *Value =
        Store ? llvm::dyn_cast<llvm::ConstantInt>(Store->getValueOperand())
              : nullptr;
    if (!Store || Store->getFunction() != &Parent || Store->isAtomic() ||
        !Value || Value->getBitWidth() != 8 * Receipt.Width ||
        Value->getZExtValue() != Receipt.Value || !Seen.insert(Store).second ||
        !Store->getNextNode())
      return std::nullopt;
    std::optional<size_t> Owner;
    for (size_t I = 0; I != Requests.size(); ++I)
      if (llvm::is_contained(Requests[I].Blocks, Store->getParent())) {
        if (Owner)
          return std::nullopt;
        Owner = I;
      }
    if (!Owner)
      return std::nullopt;
    SEHCallbackStateWrite Write{Store, *Owner, {}};
    for (int32_t Level : Bound.TargetLevels) {
      SEHCallbackStateTarget Target{
          Level, {}, Bound.FirstWriteID + Write.Targets.size()};
      if (Level >= 0) {
        const auto Token = windows_eh_semantics::getSEHScopeSemanticToken(
            *Source.ExceptionMetadata, Arch::X86, Level);
        if (!Token)
          return std::nullopt;
        Target.Digest = Token->Digest;
      } else if (Level !=
                 *Source.ExceptionMetadata->Registration->SeededTryLevel)
        return std::nullopt;
      Write.Targets.push_back(Target);
    }
    Result.push_back(std::move(Write));
  }
#endif
  return Result;
}

void emitSEHCallbackStateWrites(
    llvm::ArrayRef<SEHCallbackStateWrite> Writes,
    llvm::ArrayRef<llvm::Function *> Callbacks,
    llvm::ArrayRef<X86RegistrationCallbackInstructionMap> InstructionMaps,
    llvm::Function &Parent) {
#ifdef LLVM_NEVERD_X86_REGISTRATION_SEH_STATE_WRITES
  for (const auto &Write : Writes) {
    auto *Callback = Callbacks[Write.Callback];
    const auto *Frame = getX86RegistrationSEHJumpParentFrame(*Callback, Parent);
    auto *Store = llvm::cast<llvm::StoreInst>(
        InstructionMaps[Write.Callback].at(Write.Original));
    assert(Frame && Store->getFunction() == Callback &&
           "preflighted state store must retain its exact callback and frame");
    emitSEHCallbackStateDispatch(*Store, Write.Targets, Parent,
                                 *const_cast<llvm::Value *>(Frame));
  }
#else
  assert(Writes.empty() && "state stores require the LLVM write protocol");
#endif
}

void emitSEHRetainedScopes(llvm::Function &Parent, va_t SourceVA,
                           llvm::ArrayRef<uint32_t> Scopes,
                           llvm::ArrayRef<llvm::BasicBlock *> Unwinds) {
  if (Scopes.empty())
    return;
  auto &Setup = Parent.getEntryBlock();
  auto *Branch = Setup.getTerminator();
  assert((llvm::isa<llvm::UncondBrInst>(Branch) ||
          llvm::isa<llvm::CondBrInst>(Branch)) &&
         "checked entry must branch to its normal source path");
  auto *Next = Branch->getSuccessor(0);
  auto *NoOp = llvm::Intrinsic::getOrInsertDeclaration(
      Parent.getParent(), llvm::Intrinsic::donothing);
  for (uint32_t Scope : llvm::reverse(Scopes)) {
    auto *Block = llvm::BasicBlock::Create(
        Parent.getContext(), "registration.retain", &Parent, Next);
    llvm::IRBuilder<> B(Block);
    auto *Invoke = B.CreateInvoke(NoOp, Next, Unwinds[Scope]);
    Invoke->setMetadata(
        windows_eh_md::RegistrationScopeRetentionAttachment,
        llvm::MDNode::get(Parent.getContext(),
                          {llvm::ConstantAsMetadata::get(B.getInt64(SourceVA)),
                           llvm::ConstantAsMetadata::get(B.getInt32(Scope))}));
    Next = Block;
  }
  Branch->setSuccessor(0, Next);
}

} // namespace neverd::x86_registration
