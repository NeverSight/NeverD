//===- MedLLVMRegistrationIncoming.cpp - PE32 caller frame projection ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "MedLLVMRegistrationIncoming.h"

#include "../eh/MedLLVMEHHelpers.h"

#include "neverd/Limits.h"

#include "llvm/IR/IntrinsicInst.h"

#include <set>

namespace neverd::x86_registration {
std::optional<std::vector<IncomingFrameAccess>> prepareIncomingFrame(
    const MedFunc &Source, llvm::Function &Parent,
    const std::map<std::pair<va_t, int>, llvm::Instruction *> &Instructions) {
  if (!Source.RegistrationStates ||
      !Source.RegistrationStates->IncomingFrameAccessesComplete)
    return std::nullopt;
  std::vector<IncomingFrameAccess> Result;
  std::set<llvm::Instruction *> Seen;
  size_t Work = 0;
  for (const auto &Block : Source.Blocks)
    for (const auto &Op : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      if (Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE)
        if (const auto *Access = Source.RegistrationStates->incomingFrameAccess(
                Op.Addr, Op.OriginSeq)) {
          auto I = Instructions.find({Access->Address, Access->OpSeq});
          auto *IR = I == Instructions.end() ? nullptr : I->second;
          auto *Load = llvm::dyn_cast_or_null<llvm::LoadInst>(IR);
          auto *Store = llvm::dyn_cast_or_null<llvm::StoreInst>(IR);
          if (Op.OriginSeq < 0 ||
              Op.MemoryAddressSpace != NdMemoryAddressSpace::Default || !IR ||
              IR->getFunction() != &Parent || !Seen.insert(IR).second ||
              Access->Offset < 4 || !Access->Width ||
              int64_t(Access->Offset) + Access->Width > INT32_MAX ||
              Access->Write != (Op.Opcode == NdOp::STORE) ||
              (Access->Write ? !Store || Store->isAtomic()
                             : !Load || Load->isAtomic()))
            return std::nullopt;
          const auto Size =
              Parent.getParent()->getDataLayout().getTypeStoreSize(
                  Load ? Load->getType() : Store->getValueOperand()->getType());
          if (Size.isScalable() || Size.getFixedValue() != Access->Width)
            return std::nullopt;
          Result.push_back({IR, &Op, Access});
        }
    }
  if (Result.size() != Instructions.size())
    return std::nullopt;
  return Result;
}

IncomingFrameProjection::IncomingFrameProjection(
    llvm::Function &Parent, va_t Owner,
    llvm::ArrayRef<IncomingFrameAccess> Accesses)
    : Parent(Parent), PreviousFrameAddress(Parent.getParent()->getFunction(
                          "llvm.frameaddress.p0")) {
  if (Accesses.empty())
    return;
  auto &Context = Parent.getContext();
  llvm::IRBuilder<> Entry(Parent.getEntryBlock().getTerminator());
  Slot = Entry.CreateAlloca(Entry.getPtrTy(), nullptr,
                            "registration.caller.frame");
  Setup.push_back(Slot);
  Slot->setMetadata(
      windows_eh_md::RegistrationCallerFrameAttachment,
      llvm::MDNode::get(Context, {med_llvm_eh::mdUInt(Context, Owner, 64)}));
  auto *Frame =
      Entry.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                           Parent.getParent(), llvm::Intrinsic::frameaddress,
                           {Entry.getPtrTy()}),
                       {Entry.getInt32(0)});
  Setup.push_back(Frame);
  Setup.push_back(Entry.CreateStore(Frame, Slot));
  for (const auto &Bound : Accesses) {
    auto *IR = Bound.IR;
    const auto &Access = *Bound.Access;
    auto *Load = llvm::dyn_cast<llvm::LoadInst>(IR);
    auto *Store = llvm::dyn_cast<llvm::StoreInst>(IR);
    llvm::IRBuilder<> B(IR);
    auto *Base = B.CreateLoad(B.getPtrTy(), Slot, "registration.caller.base");
    Setup.push_back(Base);
    auto *Pointer = llvm::cast<llvm::Instruction>(
        B.CreateGEP(B.getInt8Ty(), Base, B.getInt32(Access.Offset),
                    "registration.incoming"));
    Setup.push_back(Pointer);
    Originals.push_back(
        {IR, Load ? Load->getPointerOperand() : Store->getPointerOperand(),
         IR->getMetadata(windows_eh_md::RegistrationIncomingFrameAttachment),
         Load ? Load->isVolatile() : Store->isVolatile()});
    if (Load)
      Load->setVolatile(true);
    else
      Store->setVolatile(true);
    IR->setOperand(Load ? 0 : 1, Pointer);
    IR->setMetadata(
        windows_eh_md::RegistrationIncomingFrameAttachment,
        llvm::MDNode::get(Context,
                          {med_llvm_eh::mdUInt(Context, Owner, 64),
                           med_llvm_eh::mdUInt(Context, Access.Address, 64),
                           med_llvm_eh::mdUInt(Context, Access.OpSeq, 32),
                           med_llvm_eh::mdUInt(Context, Access.Offset, 32),
                           med_llvm_eh::mdUInt(Context, Access.Width, 16),
                           med_llvm_eh::mdUInt(Context, Access.Write, 1)}));
  }
}

IncomingFrameProjection::~IncomingFrameProjection() { rollback(); }

void IncomingFrameProjection::rollback() {
  if (Committed)
    return;
  for (const auto &Original : Originals) {
    auto *Load = llvm::dyn_cast<llvm::LoadInst>(Original.IR);
    if (Load)
      Load->setVolatile(Original.Volatile);
    else
      llvm::cast<llvm::StoreInst>(Original.IR)->setVolatile(Original.Volatile);
    Original.IR->setOperand(Load ? 0 : 1, Original.Pointer);
    Original.IR->setMetadata(windows_eh_md::RegistrationIncomingFrameAttachment,
                             Original.Marker);
  }
  for (auto *Instruction : llvm::reverse(Setup))
    Instruction->eraseFromParent();
  if (!PreviousFrameAddress)
    if (auto *Frame = Parent.getParent()->getFunction("llvm.frameaddress.p0");
        Frame && Frame->use_empty())
      Frame->eraseFromParent();
  Committed = true;
  Slot = nullptr;
}
} // namespace neverd::x86_registration
