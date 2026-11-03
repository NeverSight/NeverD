//===- X86RegistrationSEHStateStore.cpp - Computed state proofs -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared validation of computed source states and native registration writes.

#include "neverd/backend/llvm/X86RegistrationSEHStateStore.h"

#include "neverd/Limits.h"
#include "neverd/backend/llvm/RegistrationFrameProofAddress.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/X86/X86RegistrationSEHStateStore.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"

#include <climits>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace neverd {

bool isX86RegistrationSEHStateValue(const llvm::Value &Value,
                                    uint32_t Expected) {
  if (!Value.getType()->isIntegerTy(32))
    return false;
  if (const auto *Literal = llvm::dyn_cast<llvm::ConstantInt>(&Value))
    return Literal->getZExtValue() == Expected;
  // Preserve the actual RMW load and flags. Only an absorbing bitwise
  // operand proves a result without a separate reaching-memory proof.
  const auto *Bits = llvm::dyn_cast<llvm::BinaryOperator>(&Value);
  if (!Bits || Bits->hasPoisonGeneratingFlags())
    return false;
  const bool Clear =
      Bits->getOpcode() == llvm::Instruction::And && Expected == 0;
  const bool Set =
      Bits->getOpcode() == llvm::Instruction::Or && Expected == UINT32_MAX;
  return (Clear || Set) &&
         llvm::any_of(Bits->operands(), [&](const llvm::Use &Operand) {
           const auto *Literal = llvm::dyn_cast<llvm::ConstantInt>(Operand);
           return Literal && Literal->getZExtValue() == Expected;
         });
}

namespace {

bool hasStateValue(const llvm::Value &Value, uint32_t Expected,
                   const llvm::AllocaInst &Frame, int64_t StateOffset,
                   registration_frame::AddressProof &Addresses, size_t &Work) {
  using Key = std::pair<const llvm::Value *, bool>;
  std::vector<std::pair<Key, bool>> Pending{{{&Value, true}, false}};
  std::set<Key> Visiting, Done;
  while (!Pending.empty()) {
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return false;
    const auto [Identity, Exit] = Pending.back();
    const auto [Current, Exact] = Identity;
    Pending.pop_back();
    if (Exit) {
      Visiting.erase(Identity);
      Done.insert(Identity);
      continue;
    }
    if (Done.count(Identity))
      continue;
    if (!Visiting.insert(Identity).second)
      return false;
    Pending.emplace_back(Identity, true);
    if (const auto *Literal = llvm::dyn_cast<llvm::ConstantInt>(Current);
        Literal && Literal->getBitWidth() == 32 &&
        (!Exact || Literal->getZExtValue() == Expected))
      continue;
    if (Exact && isX86RegistrationSEHStateValue(*Current, Expected)) {
      const auto *Bits = llvm::cast<llvm::BinaryOperator>(Current);
      for (const auto &Operand : Bits->operands())
        Pending.push_back({{Operand.get(), false}, false});
      continue;
    }
    // Before native outlining, SSA values live in compiler-owned private
    // allocas. Rebind their complete reaching stores, never an assumed value
    // in the logical source frame. The iterative walk rejects unseeded cycles.
    const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Current);
    if (!Exact && Load && !Load->isAtomic() &&
        Load->getType()->isIntegerTy(32) &&
        Addresses.locate(Load->getPointerOperand()) ==
            std::optional<registration_frame::Cell>{{&Frame, StateOffset}})
      continue;
    const auto *Slot =
        Load ? llvm::dyn_cast<llvm::AllocaInst>(Load->getPointerOperand())
             : nullptr;
    if (!Load || Load->isAtomic() || !Load->getType()->isIntegerTy(32) ||
        !Slot || Slot == &Frame || Slot->getFunction() != Frame.getFunction() ||
        !Slot->isStaticAlloca() || !Slot->getAllocatedType()->isIntegerTy(32) ||
        !llvm::isa<llvm::ConstantInt>(Slot->getArraySize()) ||
        !llvm::cast<llvm::ConstantInt>(Slot->getArraySize())->isOne())
      return false;
    for (const auto *User : Slot->users()) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return false;
      const auto *Read = llvm::dyn_cast<llvm::LoadInst>(User);
      const auto *Write = llvm::dyn_cast<llvm::StoreInst>(User);
      if (Read ? Read->getPointerOperand() != Slot || Read->isAtomic() ||
                     !Read->getType()->isIntegerTy(32)
               : !Write || Write->getPointerOperand() != Slot ||
                     Write->isAtomic() ||
                     !Write->getValueOperand()->getType()->isIntegerTy(32))
        return false;
    }
    if (!Addresses.visitReachingStores(
            *Load, [&](const llvm::StoreInst &Store) {
              if (++Work > limits::kMaxRegistrationEHStateWork)
                return false;
              Pending.push_back({{Store.getValueOperand(), Exact}, false});
              return true;
            }))
      return false;
  }
  return true;
}

} // namespace

bool hasCheckedX86RegistrationSEHComputedStores(
    const MedFunc &Source, const llvm::Function &Parent,
    const llvm::AllocaInst &Frame, uint64_t EntrySP,
    llvm::function_ref<const llvm::Instruction *(va_t, int)> Lookup) {
  if (!Source.RegistrationStates || !Source.ExceptionMetadata ||
      !Source.ExceptionMetadata->Registration)
    return false;
  const auto &Chain = *Source.ExceptionMetadata->Registration;
  std::set<va_t> Observed;
  size_t Work = 0;
  for (const auto &Store : Chain.TryLevelStores) {
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return false;
    Observed.insert(Store.StoreVA);
  }
  std::set<std::pair<va_t, int>> Computed;
  for (const auto &Store : Source.RegistrationStates->SEHStateStores) {
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return false;
    if (!Observed.count(Store.Address))
      Computed.emplace(Store.Address, Store.OpSeq);
  }
  if (Computed.empty())
    return true;
  const auto Bindings = getX86RegistrationSEHStateStores(Source, true);
  if (!Bindings || !Chain.TryLevelOffset || EntrySP > UINT32_MAX ||
      Frame.getFunction() != &Parent)
    return false;
  const int64_t Offset = int64_t(EntrySP) - 4 + *Chain.TryLevelOffset;
  coff_registration::RegistrationFrameStores Stores;
  if (!Stores.build({&Parent}, Work))
    return false;
  bool Exhausted = false;
  registration_frame::AddressProof Addresses(Parent, nullptr, 0, Stores, Work,
                                             Exhausted);
  if (!Addresses.hasPE32Layout())
    return false;
  std::map<std::pair<va_t, int>, const llvm::Instruction *> Operations;
  if (!Lookup)
    for (const auto &Block : Parent)
      for (const auto &I : Block) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return false;
        const auto *MD =
            I.getMetadata(windows_eh_md::RegistrationOperationAttachment);
        if (!MD)
          continue;
        if (MD->getNumOperands() != 5)
          return false;
        const auto *Address =
            llvm::mdconst::dyn_extract<llvm::ConstantInt>(MD->getOperand(1));
        const auto *Seq =
            llvm::mdconst::dyn_extract<llvm::ConstantInt>(MD->getOperand(2));
        if (!Address || Address->getBitWidth() != 64 || !Seq ||
            Seq->getBitWidth() != 32 || Seq->getZExtValue() > INT_MAX ||
            !Operations
                 .emplace(std::make_pair(Address->getZExtValue(),
                                         int(Seq->getZExtValue())),
                          &I)
                 .second)
          return false;
      }
  for (const auto &Binding : *Bindings) {
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return false;
    const auto &Receipt = *Binding.Source;
    const auto Key = std::make_pair(Receipt.Address, Receipt.OpSeq);
    if (!Computed.erase(Key))
      continue;
    const auto *I =
        Lookup ? Lookup(Key.first, Key.second)
               : (Operations.count(Key) ? Operations.at(Key) : nullptr);
    const auto *Store = llvm::dyn_cast_or_null<llvm::StoreInst>(I);
    if (!Store || Store->getFunction() != &Parent || Store->isAtomic() ||
        (!Lookup && !Store->isVolatile()) ||
        Store->getAlign() != llvm::Align(1) ||
        !Store->getValueOperand()->getType()->isIntegerTy(32) ||
        Addresses.locate(Store->getPointerOperand()) !=
            std::optional<registration_frame::Cell>{{&Frame, Offset}})
      return false;
    if (!hasStateValue(*Store->getValueOperand(), Receipt.Value, Frame, Offset,
                       Addresses, Work))
      return false;
  }
  return Computed.empty() && !Exhausted;
}

} // namespace neverd
