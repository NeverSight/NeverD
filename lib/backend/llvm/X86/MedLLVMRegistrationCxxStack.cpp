//===- MedLLVMRegistrationCxxStack.cpp - Private catch stack lowering -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MedLLVMRegistrationCxxStack.h"

#include "../eh/MedLLVMEHHelpers.h"

#include "neverd/backend/llvm/X86RegistrationCatchStack.h"
#include "neverd/ir/med/MedStackAlignment.h"

#include "llvm/Support/Errc.h"

namespace neverd {

llvm::Expected<RegistrationCxxStackPlan> prepareRegistrationCxxStack(
    const MedBlock &Source, llvm::BasicBlock &Entry,
    llvm::ArrayRef<llvm::BasicBlock *> Body,
    const std::map<std::pair<int, int>, llvm::AllocaInst *> &Slots,
    llvm::ArrayRef<llvm::StoreInst *> Stores) {
  RegistrationCxxStackPlan Plan;
  for (const auto &Op : Source.Ops) {
    if (Op.RegistrationRoot !=
        MedOp::RegistrationRootKind::CallbackStackPointer)
      continue;
    const auto Slot = Slots.find({Op.Output.Id, Op.Output.SSAVer});
    if (!hasValidRegistrationRootShape(Op) || Op.Output.SSAVer <= 0 ||
        Op.Addr != Source.StartAddr || Slot == Slots.end() || Plan.Seed)
      return llvm::createStringError(llvm::errc::invalid_argument,
                                     "catch stack lost its MedIR seed");
    for (auto *User : Slot->second->users())
      if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(User);
          Store && Store->getParent() == &Entry) {
        if (Plan.Seed)
          return llvm::createStringError(llvm::errc::invalid_argument,
                                         "catch stack has multiple seeds");
        Plan.Seed = Store;
      }
  }
  if (!Plan.Seed)
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "catch stack has no runtime ESP seed");
  auto Bytes = checkX86RegistrationCatchStack(Entry, Body, *Plan.Seed, Stores);
  if (!Bytes)
    return Bytes.takeError();
  Plan.Bytes = *Bytes;
  return Plan;
}

void emitRegistrationCxxStack(const RegistrationCxxStackPlan &Plan,
                              va_t FunctionVA, va_t CallbackVA) {
  auto &Parent = *Plan.Seed->getFunction();
  auto &Context = Parent.getContext();
  llvm::IRBuilder<> Entry(Parent.getEntryBlock().getTerminator());
  auto *Stack =
      Entry.CreateAlloca(llvm::ArrayType::get(Entry.getInt8Ty(), Plan.Bytes),
                         nullptr, "registration.catch.stack");
  Stack->setAlignment(llvm::Align(16));
  Stack->setMetadata(
      windows_eh_md::RegistrationCatchStackAttachment,
      llvm::MDNode::get(Context,
                        {med_llvm_eh::mdUInt(Context, FunctionVA, 64),
                         med_llvm_eh::mdUInt(Context, CallbackVA, 64)}));
  llvm::IRBuilder<> B(Plan.Seed);
  auto *Address =
      B.CreateInBoundsGEP(B.getInt8Ty(), Stack, B.getInt32(Plan.Bytes));
  Plan.Seed->setOperand(0, B.CreatePtrToInt(Address, B.getInt32Ty()));
  Plan.Seed->setMetadata(
      windows_eh_md::RegistrationRootAttachment,
      llvm::MDNode::get(Context,
                        {med_llvm_eh::mdUInt(Context, 1, 8),
                         med_llvm_eh::mdUInt(Context, Plan.Bytes, 64)}));
}

} // namespace neverd
