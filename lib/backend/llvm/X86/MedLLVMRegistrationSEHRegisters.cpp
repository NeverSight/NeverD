//===- MedLLVMRegistrationSEHRegisters.cpp - Finally register state ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keeps ordinary finally data registers in private parent-frame storage.

#include "MedLLVMRegistrationSEHRegisters.h"

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/ir/med/X86/X86RegistrationSEHRegisters.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"

#include <stdexcept>

namespace neverd {

llvm::Value *
MedLLVMEmitter::emitSEHRegisterIntrinsic(const MedOp &Op,
                                         llvm::IRBuilder<> &Builder) {
  using namespace x86_registration;
  if (TargetArch != Arch::X86 || !CurMedFunc ||
      !CurMedFunc->RegistrationStates || !hasValidSEHRegisterShape(Op))
    throw std::invalid_argument("invalid x86 SEH register exchange");
  const va_t Entry = Op.Inputs[2].ConstVal;
  const uint64_t Register = Op.Inputs[4].ConstVal;
  const auto Metadata = [&](uint64_t Role, va_t Site) {
    const auto Word = [&](uint64_t Value) {
      return llvm::ConstantAsMetadata::get(Builder.getInt64(Value));
    };
    return llvm::MDNode::get(*Ctx, {Word(CurMedFunc->Entry), Word(Role),
                                    Word(Entry), Word(Site), Word(Register)});
  };
  auto *&Slot = SEHRegisterSlots[{Entry, Register}];
  if (!Slot) {
    llvm::IRBuilder<> Allocation(&CurFunc->getEntryBlock(),
                                 CurFunc->getEntryBlock().begin());
    Slot = Allocation.CreateAlloca(Builder.getInt32Ty(), nullptr,
                                   "registration.register");
    Slot->setAlignment(llvm::Align(4));
    Slot->setMetadata(windows_eh_md::RegistrationRegisterAttachment,
                      Metadata(4, 0));
  }
  llvm::Instruction *Access = nullptr;
  if (Op.Inputs[0].ConstVal == uint64_t(Intrinsic::X86SEHRegisterRead)) {
    auto *Load = Builder.CreateLoad(Builder.getInt32Ty(), Slot,
                                    "registration.register.value");
    Load->setVolatile(true);
    Access = Load;
  } else {
    auto *Store = Builder.CreateStore(getVar(Op.Inputs[5], Builder), Slot);
    Store->setVolatile(true);
    Access = Store;
  }
  Access->setMetadata(windows_eh_md::RegistrationRegisterAttachment,
                      Metadata(Op.Inputs[1].ConstVal, Op.Addr));
  return Access->getType()->isVoidTy() ? nullptr : Access;
}

namespace x86_registration {

void bindSEHRegisterCallbackInputs(llvm::ArrayRef<llvm::Function *> Callbacks) {
  for (auto *Callback : Callbacks) {
    if (Callback->arg_size() != 2)
      continue;
    for (auto &Block : *Callback)
      for (auto &Instruction : llvm::make_early_inc_range(Block)) {
        auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction);
        const auto *MD = Instruction.getMetadata(
            windows_eh_md::RegistrationRegisterAttachment);
        if (!Load || !MD || MD->getNumOperands() != 5)
          continue;
        const auto *Role =
            llvm::mdconst::dyn_extract<llvm::ConstantInt>(MD->getOperand(1));
        if (!Role ||
            Role->getZExtValue() != uint64_t(SEHRegisterRole::CallbackInput))
          continue;
        llvm::IRBuilder<> Builder(Load->getNextNode());
        auto *Runtime =
            Builder.CreateICmpNE(Callback->getArg(0), Builder.getInt8(0));
        auto *Unknown =
            Builder.CreateFreeze(llvm::UndefValue::get(Builder.getInt32Ty()),
                                 "registration.runtime.register");
        auto *Value = llvm::cast<llvm::SelectInst>(Builder.CreateSelect(
            Runtime, Unknown, Load, "registration.callback.register"));
        Load->replaceUsesWithIf(Value, [&](const llvm::Use &Use) {
          return Use.getUser() != Value;
        });
      }
  }
}

} // namespace x86_registration
} // namespace neverd
