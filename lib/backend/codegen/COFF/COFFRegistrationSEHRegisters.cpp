//===- COFFRegistrationSEHRegisters.cpp - Private register exchange ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Proves that each finally register has one private recovered parent slot.

#include "COFFRegistrationSEHRegisters.h"

#include "COFFRegistrationCopySlots.h"

#include "neverd/Limits.h"
#include "neverd/ir/med/X86/X86RegistrationSEHRegisters.h"

namespace neverd::coff_registration {
namespace {
using x86_registration::SEHRegisterRole;
using SlotKey = std::pair<va_t, uint64_t>;

bool hasRuntimeGuard(const llvm::LoadInst &Load) {
  if (!Load.hasOneUse())
    return false;
  const auto *Select = llvm::dyn_cast<llvm::SelectInst>(*Load.user_begin());
  const auto *Compare =
      Select ? llvm::dyn_cast<llvm::ICmpInst>(Select->getCondition()) : nullptr;
  const auto *Unknown =
      Select ? llvm::dyn_cast<llvm::FreezeInst>(Select->getTrueValue())
             : nullptr;
  const auto *Owner = Load.getFunction();
  return Select && Select->getFalseValue() == &Load && Compare &&
         Compare->getPredicate() == llvm::CmpInst::ICMP_NE &&
         Owner->arg_size() == 2 && Compare->getOperand(0) == Owner->getArg(0) &&
         integer(Compare->getOperand(1), 8) == 0 && Unknown &&
         llvm::isa<llvm::UndefValue>(Unknown->getOperand(0)) &&
         !llvm::isa<llvm::PoisonValue>(Unknown->getOperand(0)) &&
         Unknown->getType()->isIntegerTy(32);
}
} // namespace

bool matchesSEHRegisterExchange(const llvm::Instruction &Instruction,
                                const MedOp &Source, va_t Owner) {
  if (!x86_registration::hasValidSEHRegisterShape(Source))
    return false;
  const auto *MD =
      Instruction.getMetadata(windows_eh_md::RegistrationRegisterAttachment);
  if (!MD || MD->getNumOperands() != 5 || metadataInteger(*MD, 0, 64) != Owner)
    return false;
  for (unsigned I = 1; I != 5; ++I)
    if (metadataInteger(*MD, I, 64) != Source.Inputs[I].ConstVal)
      return false;
  if (Source.Inputs[0].ConstVal == uint64_t(Intrinsic::X86SEHRegisterRead)) {
    const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction);
    return Load && Load->getType()->isIntegerTy(32) && Load->isVolatile() &&
           !Load->isAtomic() && Load->getAlign() == llvm::Align(4) &&
           (Source.Inputs[1].ConstVal !=
                uint64_t(SEHRegisterRole::CallbackInput) ||
            hasRuntimeGuard(*Load));
  }
  const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction);
  return Store && Store->isVolatile() && !Store->isAtomic() &&
         Store->getAlign() == llvm::Align(4) &&
         Store->getValueOperand()->getType()->isIntegerTy(32) &&
         (!Source.Inputs[5].isConst() ||
          integer(Store->getValueOperand(), 32) == Source.Inputs[5].ConstVal);
}

llvm::Error validateSEHRegisterStorage(
    const llvm::Function &Parent, const MedFunc &Source,
    const std::map<CallbackKey, const llvm::Function *> &Callbacks) {
  size_t Work = 0;
  const auto Charge = [&] {
    return ++Work <= limits::kMaxRegistrationEHStateWork;
  };
  std::set<SlotKey> Expected;
  for (const auto &Block : Source.Blocks)
    for (const auto &Op : Block.Ops) {
      if (!Charge())
        return rejectIR("finally register source exceeds its work budget");
      if (x86_registration::isSEHRegisterIntrinsic(Op)) {
        if (!x86_registration::hasValidSEHRegisterShape(Op))
          return rejectIR("finally register source is malformed");
        Expected.emplace(Op.Inputs[2].ConstVal, Op.Inputs[4].ConstVal);
      }
    }
  std::vector<const llvm::Function *> Functions;
  for (const auto &[Key, Callback] : Callbacks)
    Functions.push_back(Callback);
  RegistrationCopySlots Storage;
  if (!Storage.build(Parent, Functions, Work))
    return rejectIR("finally registers lost their recovered parent frame");
  Functions.push_back(&Parent);
  std::map<SlotKey, const llvm::AllocaInst *> Slots;
  std::map<const llvm::Instruction *, SlotKey> Accesses;
  for (const auto &Function : *Parent.getParent())
    for (const auto &Block : Function)
      for (const auto &I : Block) {
        if (!Charge())
          return rejectIR("finally register closure exceeds its work budget");
        const auto *MD =
            I.getMetadata(windows_eh_md::RegistrationRegisterAttachment);
        if (!MD || (!llvm::is_contained(Functions, &Function) &&
                    metadataInteger(*MD, 0, 64) != Source.Entry))
          continue;
        const auto Role = metadataInteger(*MD, 1, 64);
        const auto Entry = metadataInteger(*MD, 2, 64);
        const auto Site = metadataInteger(*MD, 3, 64);
        const auto Register = metadataInteger(*MD, 4, 64);
        if (MD->getNumOperands() != 5 ||
            metadataInteger(*MD, 0, 64) != Source.Entry || !Role || *Role > 4 ||
            !Entry || !Site || !Register ||
            !Expected.count({*Entry, *Register}) ||
            !llvm::is_contained(Functions, &Function))
          return rejectIR("finally register storage changed its identity");
        const SlotKey Key{*Entry, *Register};
        if (*Role == 4) {
          const auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(&I);
          if (*Site || !Slot || &Block != &Parent.getEntryBlock() ||
              !Slot->isStaticAlloca() || Slot->isArrayAllocation() ||
              Slot->isSwiftError() || Slot->isUsedWithInAlloca() ||
              Slot->getAddressSpace() ||
              !Slot->getAllocatedType()->isIntegerTy(32) ||
              Slot->getAlign() != llvm::Align(4) ||
              !Slots.emplace(Key, Slot).second)
            return rejectIR("finally register has no unique private i32 slot");
        } else {
          if (!*Site || (!llvm::isa<llvm::LoadInst, llvm::StoreInst>(I)))
            return rejectIR("finally register access lost its source site");
          if (*Role == uint64_t(SEHRegisterRole::CallbackInput) ||
              *Role == uint64_t(SEHRegisterRole::CallbackOutput)) {
            const auto Callback = Callbacks.find({*Entry, true});
            if (Callback == Callbacks.end() || Callback->second != &Function)
              return rejectIR("finally register belongs to another callback");
          }
          Accesses.emplace(&I, Key);
        }
      }
  if (Slots.size() != Expected.size())
    return rejectIR("finally register storage set is incomplete");
  std::set<const llvm::Instruction *> Seen;
  for (const auto &[Key, Slot] : Slots)
    for (const auto *Alias : Storage.aliases(Slot))
      for (const auto *User : Alias->users()) {
        if (!Charge())
          return rejectIR("finally register aliases exceed their work budget");
        if (Alias == Slot && Storage.isEscape(User))
          continue;
        const auto *I = llvm::dyn_cast<llvm::Instruction>(User);
        const auto Access = Accesses.find(I);
        if (!I || Access == Accesses.end() || Access->second != Key ||
            !Seen.insert(I).second)
          return rejectIR("finally register slot escapes or changes registers");
        const auto *Load = llvm::dyn_cast<llvm::LoadInst>(I);
        const auto *Store = llvm::dyn_cast<llvm::StoreInst>(I);
        if ((Load && Load->getPointerOperand() != Alias) ||
            (Store && Store->getPointerOperand() != Alias))
          return rejectIR("finally register address escapes through a value");
      }
  if (Seen.size() != Accesses.size())
    return rejectIR("finally register access names an unrelated allocation");
  return llvm::Error::success();
}

} // namespace neverd::coff_registration
